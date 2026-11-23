#!/usr/bin/env python3
"""Exercise the production orientation callback across a legacy SDK boundary.

The macOS fixture delivers the same error on the main queue and a worker queue.
It does not simulate UIKit orientation changes or test a particular iOS device.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path,
                    default=repo / 'platform/ipados/App/CemuIOSApp.swift')
args = parser.parse_args()
source = args.source.read_text()


def declaration(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


registration = declaration(source, 'private static func apply(')
registration = registration[registration.index('scene.requestGeometryUpdate'):]
# Keep the actual production registration and handler, replacing only UIKit's
# geometry argument with the fixture's value type.
registration = registration[:registration.rfind('}')]
callback = ''
if 'nonisolated private static func logOrientationRequestError(' in source:
    callback = declaration(source, 'nonisolated private static func logOrientationRequestError(')

sdk = r'''
import Foundation
import Dispatch

public struct Geometry { public static func iOS(interfaceOrientations: Int) -> Geometry { Geometry() } }
public final class Scene {
    public init() {}
    public func requestGeometryUpdate(_ geometry: Geometry, errorHandler: ((Error) -> Void)?) {
        let error = NSError(domain: "OrientationTest", code: 101,
                            userInfo: [NSLocalizedDescriptionKey: "window does not support this orientation"])
        DispatchQueue.main.async {
            errorHandler?(error)
            DispatchQueue.global().async {
                dispatchPrecondition(condition: .notOnQueue(.main))
                errorHandler?(error)
                print("PASS: production orientation error callback returned on main and worker queues")
                exit(0)
            }
        }
    }
}
public func log(_ message: String) {
    precondition(message == "[UI] Orientation request failed: window does not support this orientation")
}
'''
fixture = r'''
import Foundation
import Dispatch
import LegacyGeometry
func CemuBridge_LogJITMilestone(_ message: String) { LegacyGeometry.log(message) }
@MainActor enum GameOrientationCoordinator {
    CALLBACK
    static func run() {
        let scene = Scene(), orientations = 1
        REGISTRATION
    }
}
Task { @MainActor in GameOrientationCoordinator.run() }
dispatchMain()
'''.replace('CALLBACK', callback).replace('REGISTRATION', registration)

with tempfile.TemporaryDirectory(prefix='77u-orientation-callback-') as temp:
    p = Path(temp)
    (p / 'LegacyGeometry.swift').write_text(sdk)
    (p / 'main.swift').write_text(fixture)
    # UIKit is an imported, synchronous callback API. A Swift 5 module preserves
    # that legacy boundary instead of pretending it promises a main executor.
    subprocess.run(['xcrun', 'swiftc', '-swift-version', '5', '-emit-library', '-emit-module',
                    '-module-name', 'LegacyGeometry', str(p / 'LegacyGeometry.swift'),
                    '-o', str(p / 'libLegacyGeometry.dylib')], check=True, cwd=p)
    subprocess.run(['xcrun', 'swiftc', '-swift-version', '6', '-strict-concurrency=complete',
                    '-I', temp, '-L', temp, '-lLegacyGeometry', str(p / 'main.swift'),
                    '-o', str(p / 'test')], check=True, cwd=p)
    subprocess.run([str(p / 'test')],
                   env=dict(os.environ, DYLD_LIBRARY_PATH=temp), timeout=15, check=True)
