# SPIRV-Cross Git Subtree Setup

## Overview

SPIRV-Cross has been integrated using **git subtree** (not submodule) to match GZDoom's project conventions. This document explains the setup and how to update it in the future.

## Git Subtree vs Submodule

### Git Submodule (NOT used here)
```
✗ Requires `git submodule init` and `git submodule update`
✗ Creates .gitmodules file
✗ External repository referenced, not copied
✗ Contributors need to know about submodule commands
```

### Git Subtree (used by GZDoom)
```
✓ Code is copied directly into the repository
✓ No special commands needed for cloning
✓ Easier for contributors (just works!)
✓ Used by ZWidget, ZMusic, and now SPIRV-Cross
```

## Current Setup

SPIRV-Cross has been added as a subtree at:
```
libraries/ZVulkan/src/SPIRV-Cross/
```

**Commit history:**
```
* Add SPIRV-Cross to subtree update script
* Merge commit (SPIRV-Cross subtree)
* Squashed 'libraries/ZVulkan/src/SPIRV-Cross/' content
```

## How to Update SPIRV-Cross

### Option 1: Use the Update Script (Recommended)

```bash
# Update SPIRV-Cross only
./tools/update-subtrees.sh spirv-cross

# Or update all subtrees (ZWidget, ZMusic, SPIRV-Cross)
./tools/update-subtrees.sh all
```

### Option 2: Manual Update

```bash
# Pull latest changes from SPIRV-Cross repository
git subtree pull \
    --prefix=libraries/ZVulkan/src/SPIRV-Cross \
    https://github.com/KhronosGroup/SPIRV-Cross \
    main \
    --squash

# The --squash flag is important!
# It combines all upstream commits into a single merge commit
```

## What Gets Updated

When you run the update script:

1. **Fetches** latest SPIRV-Cross code from GitHub
2. **Merges** changes into `libraries/ZVulkan/src/SPIRV-Cross/`
3. **Creates** a squashed merge commit
4. **Preserves** local modifications (if any)

## Integration with shader-translator

The shader-translator library references SPIRV-Cross via CMake:

```cmake
# In libraries/shader-translator/CMakeLists.txt
set(SPIRV_CROSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ZVulkan/src/SPIRV-Cross")
add_subdirectory("${SPIRV_CROSS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Cross")
```

This means:
- ✅ No git submodule init needed
- ✅ Clean CMake configuration
- ✅ Builds automatically with GZDoom

## Version Information

**Current SPIRV-Cross version:**
- Branch: `main`
- Commit: `97709575e2` (vulkan-sdk-1.4.328.0-13)
- Added: November 2025

**Upstream repository:**
- URL: https://github.com/KhronosGroup/SPIRV-Cross
- Maintained by: Khronos Group
- License: Apache 2.0

## Troubleshooting

### "Working tree has modifications" Error

Git subtree commands require a clean working tree. Solution:

```bash
# Stash your changes
git stash

# Run the update
./tools/update-subtrees.sh spirv-cross

# Restore your changes
git stash pop
```

### Merge Conflicts

If SPIRV-Cross updates conflict with local changes:

```bash
# The update script will stop and report conflicts
# Resolve them manually, then:
git add libraries/ZVulkan/src/SPIRV-Cross/
git commit -m "Merge SPIRV-Cross updates"
```

### Verifying Subtree Integrity

```bash
# Check subtree status
git log --oneline libraries/ZVulkan/src/SPIRV-Cross/ | head -5

# Should show "Squashed 'libraries/ZVulkan/src/SPIRV-Cross/'" commits
```

## Future Considerations

### When NOT to Update

- During active Metal renderer development (to avoid breaking changes)
- Before major GZDoom releases (for stability)
- If SPIRV-Cross has known regressions

### When TO Update

- Before starting Metal renderer work (get latest features)
- When new Metal features require newer SPIRV-Cross
- Security updates or critical bug fixes

### Pinning to Specific Version

If you need a specific SPIRV-Cross version:

```bash
# Update to a specific commit/tag
git subtree pull \
    --prefix=libraries/ZVulkan/src/SPIRV-Cross \
    https://github.com/KhronosGroup/SPIRV-Cross \
    <commit-hash-or-tag> \
    --squash
```

## References

- [Git Subtree Documentation](https://git-scm.com/book/en/v1/Git-Tools-Subtree-Merging)
- [SPIRV-Cross GitHub](https://github.com/KhronosGroup/SPIRV-Cross)
- [GZDoom Subtree Script](../../tools/update-subtrees.sh)
