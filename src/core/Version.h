#pragma once
// Single source of truth for the engine version (also used by the .rc files,
// so keep this header plain preprocessor definitions).
#define RIFT_VERSION_MAJOR 1
#define RIFT_VERSION_MINOR 1
#define RIFT_VERSION_PATCH 0
#define RIFT_VERSION_STRING "1.1.0"

// Where releases are published (the updater reads its GitHub Releases).
#define RIFT_GITHUB_REPO "SamoVR/RiftEngine"
#define RIFT_WEBSITE "https://samovr.github.io/RiftEngine/"
#define RIFT_DOCS_URL "https://github.com/SamoVR/RiftEngine/blob/main/docs/"
