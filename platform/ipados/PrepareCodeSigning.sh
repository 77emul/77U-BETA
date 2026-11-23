#!/bin/sh

set -eu

# Xcode exports both values to build scripts. Using them avoids embedding an
# unresolved ${EFFECTIVE_PLATFORM_NAME} placeholder in the generated project.
app_bundle="${TARGET_BUILD_DIR}/${WRAPPER_NAME}"

if [ ! -d "${app_bundle}" ]; then
    echo "error: Expected app bundle was not found: ${app_bundle}" >&2
    exit 1
fi

# Copy the JIT script into the app bundle.
script_dir="$(cd "$(dirname "$0")" && pwd)"
jit_script="${script_dir}/../../dependencies/StikJIT/Resources/universal.js"
if [ ! -f "${jit_script}" ]; then
    echo "error: Missing JIT script: ${jit_script}" >&2
    exit 1
fi
if ! /usr/bin/cmp -s "${jit_script}" "${app_bundle}/universal.js"; then
    /bin/cp "${jit_script}" "${app_bundle}/universal.js"
fi

# Ensure default game profiles are present in CemuData inside the app bundle
repo_root="$(cd "${script_dir}/../.." && pwd)"
if [ -d "${repo_root}/bin/gameProfiles/default" ]; then
    mkdir -p "${app_bundle}/CemuData/gameProfiles"
    cp -R "${repo_root}/bin/gameProfiles/default" "${app_bundle}/CemuData/gameProfiles/"
fi

# Strip filesystem metadata and detritus (FinderInfo, ResourceFork, Quarantine, etc.)
# that can make ordinary resources look like unsigned nested code to codesign.
/usr/bin/xattr -cr "${app_bundle}" 2>/dev/null || true
/usr/bin/xattr -dr com.apple.provenance "${app_bundle}" 2>/dev/null || true
/usr/bin/xattr -dr com.apple.quarantine "${app_bundle}" 2>/dev/null || true
