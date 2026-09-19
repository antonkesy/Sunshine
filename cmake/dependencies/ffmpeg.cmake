#
# Loads FFmpeg pre-compiled binaries from GitHub releases or a user-specified path
#
include_guard(GLOBAL)

# Resolve a build-deps static library inside FFMPEG_PREPARED_BINARIES.
#
# The components do not agree on an install libdir. FFmpeg and the autotools
# components always land in lib/, but SVT-AV1 uses GNUInstallDirs, which resolves
# to lib64 on 64-bit non-Debian distros (Arch, Fedora, openSUSE). Probing lib/ only
# meant a locally built tree silently dropped libSvtAv1Enc.a from the link line and
# failed much later with undefined references to svt_av1_enc_* from libavcodec.a.
# CI never saw it because the release tarballs are built on Ubuntu.
#
# Sets ${out_var} to the resolved path, or the empty string when not found.
function(ffmpeg_find_prepared_lib out_var filename)
    foreach(libdir lib lib64)
        if(EXISTS "${FFMPEG_PREPARED_BINARIES}/${libdir}/${filename}")
            set(${out_var} "${FFMPEG_PREPARED_BINARIES}/${libdir}/${filename}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# ffmpeg pre-compiled binaries
if(NOT DEFINED FFMPEG_PREPARED_BINARIES)
    # Set platform-specific libraries
    if(WIN32)
        set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
    elseif(FREEBSD)
        # numa is not available on FreeBSD
        set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
    elseif(UNIX AND NOT APPLE)
        set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
    endif()

    # Determine download location
    set(FFMPEG_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps")

    # Fetch tags for the build-deps submodule so tag lookups work in CI shallow clones
    execute_process(
        COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" fetch --tags --depth=1
        OUTPUT_QUIET
        ERROR_QUIET
    )

    # Get the current commit/tag from the build-deps submodule
    execute_process(
        COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" describe --tags --exact-match
        OUTPUT_VARIABLE FFMPEG_RELEASE_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # If no exact tag match, try to get the commit hash and look for a tag
    if(NOT FFMPEG_RELEASE_TAG)
        execute_process(
            COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" rev-parse HEAD
            OUTPUT_VARIABLE BUILD_DEPS_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        # Try to find a tag that points to this commit
        execute_process(
            COMMAND git -C "${CMAKE_SOURCE_DIR}/third-party/build-deps" tag --points-at ${BUILD_DEPS_COMMIT}
            OUTPUT_VARIABLE FFMPEG_RELEASE_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()

    # Set GitHub release URL
    set(FFMPEG_GITHUB_REPO "LizardByte/build-deps")
    if(FFMPEG_RELEASE_TAG)
        set(FFMPEG_RELEASE_URL "https://github.com/${FFMPEG_GITHUB_REPO}/releases/download/${FFMPEG_RELEASE_TAG}")
        set(FFMPEG_VERSION_DIR "${FFMPEG_DOWNLOAD_DIR}/ffmpeg-${FFMPEG_RELEASE_TAG}")
        message(STATUS "Using FFmpeg from build-deps tag: ${FFMPEG_RELEASE_TAG}")
    else()
        set(FFMPEG_RELEASE_URL "https://github.com/${FFMPEG_GITHUB_REPO}/releases/latest/download")
        set(FFMPEG_VERSION_DIR "${FFMPEG_DOWNLOAD_DIR}/ffmpeg-latest")
        message(STATUS "Using FFmpeg from latest build-deps release")
    endif()

    # Set extraction directory and prepared binaries path
    set(FFMPEG_EXTRACT_DIR "${FFMPEG_DOWNLOAD_DIR}")
    set(FFMPEG_PREPARED_BINARIES "${FFMPEG_EXTRACT_DIR}/ffmpeg")

    # Set the archive filename based on architecture
    set(FFMPEG_ARCHIVE_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-ffmpeg.tar.gz")
    set(FFMPEG_ARCHIVE_PATH "${FFMPEG_VERSION_DIR}/${FFMPEG_ARCHIVE_NAME}")
    set(FFMPEG_DOWNLOAD_URL "${FFMPEG_RELEASE_URL}/${FFMPEG_ARCHIVE_NAME}")

    # Check if already downloaded and extracted
    if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
        # Check if we need to download the archive
        if(NOT EXISTS "${FFMPEG_ARCHIVE_PATH}")
            message(STATUS "Downloading FFmpeg binaries from ${FFMPEG_DOWNLOAD_URL}")

            # Download the archive
            file(DOWNLOAD
                "${FFMPEG_DOWNLOAD_URL}"
                "${FFMPEG_ARCHIVE_PATH}"
                SHOW_PROGRESS
                STATUS FFMPEG_DOWNLOAD_STATUS
                TIMEOUT 300
            )

            # Check download status
            list(GET FFMPEG_DOWNLOAD_STATUS 0 FFMPEG_DOWNLOAD_STATUS_CODE)
            list(GET FFMPEG_DOWNLOAD_STATUS 1 FFMPEG_DOWNLOAD_STATUS_MESSAGE)

            if(NOT FFMPEG_DOWNLOAD_STATUS_CODE EQUAL 0)
                message(FATAL_ERROR "Failed to download FFmpeg binaries: ${FFMPEG_DOWNLOAD_STATUS_MESSAGE}")
            endif()
        else()
            message(STATUS "Using cached FFmpeg archive at ${FFMPEG_ARCHIVE_PATH}")
        endif()

        # Extract the archive
        message(STATUS "Extracting FFmpeg binaries to ${FFMPEG_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT  # cmake-lint: disable=E1126
            INPUT "${FFMPEG_ARCHIVE_PATH}"
            DESTINATION "${FFMPEG_EXTRACT_DIR}"
        )

        # Verify extraction
        if(NOT EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a")
            message(FATAL_ERROR "FFmpeg extraction failed or unexpected directory structure")
        endif()

        message(STATUS "FFmpeg binaries successfully downloaded and extracted")
    else()
        message(STATUS "Using existing FFmpeg binaries at ${FFMPEG_PREPARED_BINARIES}")
    endif()

    # Set FFmpeg libraries
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
        set(HDR10_PLUS_LIBRARY "${FFMPEG_PREPARED_BINARIES}/lib/libhdr10plus.a")
    endif()

    # The oneVPL dispatcher is only shipped in Linux x86_64 build-deps tarballs;
    # older tarballs and other platforms do not have it.
    if(EXISTS "${FFMPEG_PREPARED_BINARIES}/lib/libvpl.a")
        set(VPL_LIBRARY "${FFMPEG_PREPARED_BINARIES}/lib/libvpl.a")
    endif()

    set(FFMPEG_LIBRARIES
        "${FFMPEG_PREPARED_BINARIES}/lib/libavcodec.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libswscale.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libavutil.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libcbs.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libSvtAv1Enc.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx264.a"
        "${FFMPEG_PREPARED_BINARIES}/lib/libx265.a"
        ${HDR10_PLUS_LIBRARY}
        ${VPL_LIBRARY}
        ${FFMPEG_PLATFORM_LIBRARIES}
    )
else()
    # User provided FFMPEG_PREPARED_BINARIES path
    message(STATUS "Using user-specified FFmpeg binaries at ${FFMPEG_PREPARED_BINARIES}")

    # Set platform-specific libraries
    if(NOT DEFINED FFMPEG_PLATFORM_LIBRARIES)
        if(WIN32)
            set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)
        elseif(FREEBSD)
            set(FFMPEG_PLATFORM_LIBRARIES va va-drm va-x11 X11)
        elseif(UNIX AND NOT APPLE)
            set(FFMPEG_PLATFORM_LIBRARIES numa va va-drm va-x11 X11)
        endif()
    endif()

    # Set base FFmpeg libraries (always required). Link order is significant for
    # static archives, so keep these ahead of the optional components below.
    set(FFMPEG_LIBRARIES "")
    foreach(ffmpeg_lib libavcodec.a libswscale.a libavutil.a libcbs.a)
        ffmpeg_find_prepared_lib(ffmpeg_lib_path "${ffmpeg_lib}")
        if(NOT ffmpeg_lib_path)
            message(FATAL_ERROR
                "Required FFmpeg library ${ffmpeg_lib} not found under "
                "${FFMPEG_PREPARED_BINARIES}/lib or ${FFMPEG_PREPARED_BINARIES}/lib64")
        endif()
        list(APPEND FFMPEG_LIBRARIES "${ffmpeg_lib_path}")
    endforeach()

    # Add optional libraries if they exist (e.g., from prebuilt packages).
    # A missing one is reported rather than skipped silently: if FFmpeg was built
    # with the matching encoder enabled, its absence here is not benign -- it shows
    # up as undefined references when linking sunshine.
    foreach(ffmpeg_lib libSvtAv1Enc.a libx264.a libx265.a libhdr10plus.a libvpl.a)
        ffmpeg_find_prepared_lib(ffmpeg_lib_path "${ffmpeg_lib}")
        if(ffmpeg_lib_path)
            list(APPEND FFMPEG_LIBRARIES "${ffmpeg_lib_path}")
        else()
            message(STATUS "Optional FFmpeg component not found, skipping: ${ffmpeg_lib}")
        endif()
    endforeach()

    # Add platform libraries
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_PLATFORM_LIBRARIES})
endif()

set(FFMPEG_INCLUDE_DIRS "${FFMPEG_PREPARED_BINARIES}/include")
