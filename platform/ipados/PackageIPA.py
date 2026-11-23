#!/usr/bin/env python3
"""Package an iOS app without debug/local symbols or macOS archive sidecars.

Only a staging copy is modified. Executable sections and exported symbols
remain identical. Signed exports preserve identity and entitlements; public
sideloading exports use a neutral bundle ID and an ad-hoc signature without
the developer's certificates or provisioning profiles. The latter require
signing by the recipient's sideloading tool before installation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile


def run(*args):
    return subprocess.check_output([str(a) for a in args], stderr=subprocess.PIPE)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sections(path):
    data = path.read_bytes()
    header = struct.unpack_from('<8I', data)
    if header[0] != 0xFEEDFACF:
        raise RuntimeError(f'Expected a thin 64-bit Mach-O: {path}')
    result = {}
    position = 32
    for _ in range(header[4]):
        command, length = struct.unpack_from('<II', data, position)
        if command == 0x19:  # LC_SEGMENT_64
            segment = struct.unpack_from('<II16sQQQQiiII', data, position)
            for i in range(segment[9]):
                section = struct.unpack_from('<16s16sQQ8I', data, position + 72 + i * 80)
                name, seg, address, size, offset = section[:5]
                flags = section[8]
                key = (seg.rstrip(b'\0').decode(), name.rstrip(b'\0').decode())
                # Zero-filled sections have no bytes on disk, but retain size/address.
                zero_filled = (flags & 0xFF) in (1, 0xC, 0x12)
                result[key] = (address, size, flags, None if zero_filled else digest(data[offset:offset + size]))
        position += length
    if not result:
        raise RuntimeError(f'No executable sections: {path}')
    return result


def exports(path):
    return sorted(run('xcrun', 'nm', '-gUj', path).splitlines())


def entitlements(bundle):
    return plistlib.loads(run('codesign', '-d', '--entitlements', ':-', bundle))


def ignored(path):
    return any(p == '.DS_Store' or p == '__MACOSX' or p.startswith('._') for p in path.parts)


def resource_hashes(app, executable_paths):
    return {p.relative_to(app).as_posix(): digest(p.read_bytes()) for p in app.rglob('*')
            if p.is_file() and not ignored(p.relative_to(app))
            and '_CodeSignature' not in p.relative_to(app).parts
            and p.relative_to(app) not in executable_paths}


def sideloading_identity(app, bundles, bundle_id, original_entitlements):
    """Change distribution metadata on the staging copy, never the local app."""
    parts = bundle_id.split('.')
    if len(parts) < 2 or any(not part or not all(c.isascii() and (c.isalnum() or c == '-')
                                               for c in part) for part in parts):
        raise RuntimeError('Use a reverse-DNS bundle identifier with letters, numbers, dots and hyphens')
    original_id = plistlib.loads((app / 'Info.plist').read_bytes())['CFBundleIdentifier']
    clean_entitlements = []
    for bundle, values in zip(bundles, original_entitlements):
        if values.get('com.apple.security.application-groups'):
            raise RuntimeError('App groups need an explicit public signing/migration policy')
        info_path = bundle / 'Info.plist'
        info = plistlib.loads(info_path.read_bytes())
        old = info['CFBundleIdentifier']
        if old != original_id and not old.startswith(original_id + '.'):
            raise RuntimeError(f'Unexpected extension identifier: {old}')
        info['CFBundleIdentifier'] = bundle_id + old[len(original_id):]
        if bundle == app:
            info['CFBundleName'] = bundle_id
        info_path.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
        (bundle / 'embedded.mobileprovision').unlink(missing_ok=True)
        # The recipient's signer supplies their own team/application identity.
        # Keep actual app capabilities, including the increased memory limit.
        clean_entitlements.append({k: v for k, v in values.items() if k not in {
            'application-identifier', 'com.apple.application-identifier',
            'com.apple.developer.team-identifier', 'keychain-access-groups'}})
    return clean_entitlements


def verify_sideloading_identity(app, bundles, bundle_id):
    if list(app.rglob('embedded.mobileprovision')):
        raise RuntimeError('A private provisioning profile remains in the public app')
    for bundle in bundles:
        info = plistlib.loads((bundle / 'Info.plist').read_bytes())
        actual = info['CFBundleIdentifier']
        if actual != bundle_id and not actual.startswith(bundle_id + '.'):
            raise RuntimeError(f'Unexpected public bundle ID: {actual}')
        values = entitlements(bundle)
        if any(k in values for k in ('application-identifier', 'com.apple.application-identifier',
                                    'com.apple.developer.team-identifier', 'keychain-access-groups')):
            raise RuntimeError('Private signing identity remains in public entitlements')
        signature = subprocess.run(['codesign', '-d', '-vv', str(bundle)],
                                   check=True, capture_output=True).stderr.decode()
        if 'Signature=adhoc' not in signature or 'Authority=' in signature:
            raise RuntimeError('Public app must have an ad-hoc signature without certificates')


def verify_no_host_paths(app):
    pattern = re.compile(rb'/(?:Users|home)/[A-Za-z0-9_.-]+/|/Volumes/[^\x00\r\n/]{1,64}/')
    files = [path.relative_to(app).as_posix() for path in sorted(app.rglob('*'))
             if path.is_file() and pattern.search(path.read_bytes())]
    if files:
        raise RuntimeError('Build host paths in public app: ' + ', '.join(files))

def archive(app, destination):
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(app.rglob('*')):
            relative = path.relative_to(app.parent.parent)
            if ignored(relative):
                continue
            if path.is_symlink():
                raise RuntimeError(f'Unexpected symlink in iOS bundle: {path}')
            if path.is_file():
                z.write(path, relative.as_posix())
    with zipfile.ZipFile(destination) as z:
        if z.testzip() is not None:
            raise RuntimeError('IPA archive failed CRC verification')
        if any(ignored(Path(name)) for name in z.namelist()):
            raise RuntimeError('macOS sidecar in IPA')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('app', type=Path)
    p.add_argument('ipa', type=Path)
    signing = p.add_mutually_exclusive_group(required=True)
    signing.add_argument('--identity', help='Preserve the signed identity for a private/device export')
    signing.add_argument('--for-sideloading', action='store_true',
                         help='Remove personal signing data; recipient must re-sign before installing')
    p.add_argument('--bundle-id', default='com.77emul.app', help='Public sideloading bundle ID')
    p.add_argument('--work', type=Path, required=True)
    args = p.parse_args()
    app, ipa, work = args.app.resolve(), args.ipa.resolve(), args.work.resolve()
    if work.exists():
        raise RuntimeError('Use a new staging directory; existing work is never removed')
    run('codesign', '--verify', '--deep', '--strict', app)
    work.mkdir(parents=True)
    payload = work / 'Payload'
    payload.mkdir()
    staged = payload / app.name
    shutil.copytree(app, staged, ignore=lambda _, names: [n for n in names if ignored(Path(n))])
    bundles = sorted(staged.rglob('*.appex'), key=lambda x: len(x.parts), reverse=True) + [staged]
    if list(staged.rglob('*.framework')) or list(staged.rglob('*.dylib')):
        raise RuntimeError('Additional signed code needs explicit inside-out handling')
    executables = []
    for bundle in bundles:
        info = plistlib.loads((bundle / 'Info.plist').read_bytes())
        executables.append(bundle / info['CFBundleExecutable'])
    expected_entitlements = [entitlements(bundle) for bundle in bundles]
    if args.for_sideloading:
        expected_entitlements = sideloading_identity(staged, bundles, args.bundle_id, expected_entitlements)
    relative_executables = {x.relative_to(staged) for x in executables}
    resources = resource_hashes(staged, relative_executables)
    baseline = work / 'unstripped.ipa'
    archive(staged, baseline)
    original_bytes = sum(p.stat().st_size for p in staged.rglob('*') if p.is_file())
    binaries = []
    for index, (bundle, executable, expected) in enumerate(zip(bundles, executables, expected_entitlements)):
        before_sections, before_exports = sections(executable), exports(executable)
        original_size = executable.stat().st_size
        if args.for_sideloading:
            run('codesign', '--remove-signature', executable)
        run('xcrun', 'strip', '-S', '-x', executable)
        if sections(executable) != before_sections or exports(executable) != before_exports:
            raise RuntimeError(f'Stripping changed executable sections/exports: {executable}')
        if args.for_sideloading:
            metadata = work / f'public-entitlements-{index}.plist'
            metadata.write_bytes(plistlib.dumps(expected))
            public_id = plistlib.loads((bundle / 'Info.plist').read_bytes())['CFBundleIdentifier']
            run('codesign', '--force', '--sign', '-', '--timestamp=none',
                '--identifier', public_id, '--entitlements', metadata, bundle)
        else:
            run('codesign', '--force', '--sign', args.identity, '--timestamp=none',
                '--preserve-metadata=identifier,entitlements,requirements,flags,runtime', bundle)
        if entitlements(bundle) != expected:
            raise RuntimeError(f'Signing changed entitlements: {bundle}')
        binaries.append({'path': str(executable.relative_to(staged)), 'before_bytes': original_size,
                         'after_bytes': executable.stat().st_size, 'runtime_sections_unchanged': True,
                         'exports_unchanged': True, 'entitlements_unchanged': not args.for_sideloading,
                         'expected_entitlements_verified': True})
    run('codesign', '--verify', '--deep', '--strict', staged)
    if args.for_sideloading:
        verify_sideloading_identity(staged, bundles, args.bundle_id)
        verify_no_host_paths(staged)
    if resource_hashes(staged, relative_executables) != resources:
        raise RuntimeError('Packaging changed an app resource')
    ipa.parent.mkdir(parents=True, exist_ok=True)
    if ipa.exists():
        raise RuntimeError(f'Refusing to overwrite {ipa}')
    with tempfile.NamedTemporaryFile(dir=ipa.parent, suffix='.ipa', delete=False) as f:
        temporary = Path(f.name)
    try:
        archive(staged, temporary)
        temporary.rename(ipa)
    finally:
        temporary.unlink(missing_ok=True)
    sha = digest(ipa.read_bytes())
    ipa.with_suffix(ipa.suffix + '.sha256').write_text(f'{sha}  {ipa.name}\n')
    info = plistlib.loads((staged / 'Info.plist').read_bytes())
    result = {'ipa': ipa.name, 'staged_app': staged.relative_to(work).as_posix(), 'sha256': sha,
              'ipa_bytes': ipa.stat().st_size, 'unstripped_ipa_bytes': baseline.stat().st_size,
              'bundle_before_bytes': original_bytes,
              'bundle_after_bytes': sum(p.stat().st_size for p in staged.rglob('*') if p.is_file()),
              'resources_verified': len(resources), 'binaries': binaries,
              'minimum_os': info.get('MinimumOSVersion'), 'device_families': info.get('UIDeviceFamily'),
              'bundle_id': info['CFBundleIdentifier'], 'version': info['CFBundleShortVersionString'],
              'build': info['CFBundleVersion'], 'signature_verified': True,
              'signing_mode': 'public-sideloading' if args.for_sideloading else 'personal-signed',
              'requires_resigning': args.for_sideloading,
              'personal_signing_data_removed': args.for_sideloading}
    (work / 'package-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
