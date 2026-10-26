#pragma once
// Single source of truth for the engine version (also used by the .rc files,
// so keep this header plain preprocessor definitions).
#define RIFT_VERSION_MAJOR 0
#define RIFT_VERSION_MINOR 2
#define RIFT_VERSION_PATCH 0
#define RIFT_VERSION_STRING "0.2.0"
// What people see (title bars, About, the setup): the number plus the stage.
#define RIFT_VERSION_STAGE "Beta"
#define RIFT_VERSION_DISPLAY RIFT_VERSION_STRING " " RIFT_VERSION_STAGE

// Where releases are published (the updater reads its GitHub Releases).
#define RIFT_GITHUB_REPO "SamoVR/RiftEngine"
#define RIFT_WEBSITE "https://samovr.github.io/RiftEngine/"
#define RIFT_DOCS_URL "https://github.com/SamoVR/RiftEngine/blob/main/docs/"
