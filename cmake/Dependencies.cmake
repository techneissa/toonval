# Every third-party library is a release tarball pinned by SHA-256, so a fresh clone builds the same
# everywhere. To upgrade one, change the URL and replace the hash with `shasum -a 256` of the new file.
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW) # extracted files get the extraction time, so rebuilds work

if(TOONVAL_BUILD_APP)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.16.tar.gz
    URL_HASH SHA256=c2ee715e42ec520c4d11fd8d249ef1d2b2baf4ad31148b72b3b000276c0b3633
    SYSTEM)
  FetchContent_MakeAvailable(SDL3)

  # Text for the chord name and status lines. SDL3_ttf (zlib) needs FreeType, which its release
  # tarball leaves out, so FreeType's own release tarball is unpacked into SDL3_ttf's external/
  # folder and built as SDL3_ttf's vendored copy. FreeType is dual-licensed FTL / GPL-2.0-or-later;
  # the FTL is GPL-3.0-compatible. HarfBuzz and plutosvg are off: chord names are plain Latin text.
  # SOURCE_SUBDIR points at a missing folder so MakeAvailable only downloads, and the
  # add_subdirectory below runs once FreeType is in place.
  FetchContent_Declare(SDL3_ttf
    URL https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-3.2.2.tar.gz
    URL_HASH SHA256=63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d
    SOURCE_SUBDIR no-add-subdirectory
    SYSTEM)
  FetchContent_MakeAvailable(SDL3_ttf)
  FetchContent_Declare(freetype
    URL https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz
        https://downloads.sourceforge.net/project/freetype/freetype2/2.14.1/freetype-2.14.1.tar.xz
    URL_HASH SHA256=32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc
    SOURCE_DIR ${sdl3_ttf_SOURCE_DIR}/external/freetype
    SOURCE_SUBDIR no-add-subdirectory)
  FetchContent_MakeAvailable(freetype)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
  set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
  set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
  set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
  set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${sdl3_ttf_SOURCE_DIR} ${sdl3_ttf_BINARY_DIR} EXCLUDE_FROM_ALL SYSTEM)
  # Third-party warnings are not ours to fix.
  foreach(target freetype SDL3_ttf-static)
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  endforeach()

  # The chord-name font: Atkinson Hyperlegible Bold (SIL OFL 1.1, Braille Institute), designed to
  # stay readable at a distance. One file from the google/fonts repository at a fixed commit. It is
  # compiled into the app (app/CMakeLists.txt), so nothing has to be installed next to the binary.
  # A binary release must ship the OFL text with it.
  FetchContent_Declare(font_atkinson
    URL https://raw.githubusercontent.com/google/fonts/95f4904fc8bcf26d3420fe315560c96417c6dec7/ofl/atkinsonhyperlegible/AtkinsonHyperlegible-Bold.ttf
    URL_HASH SHA256=5a3b0c8cc8ca545155150b4512a1fa248298df121c50d6557e651e61fbdab92f
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(font_atkinson)
  set(TOONVAL_FONT_FILE ${font_atkinson_SOURCE_DIR}/AtkinsonHyperlegible-Bold.ttf)

  # The settings screens use the regular weight of the same family for body text (same commit,
  # same licence as above).
  FetchContent_Declare(font_atkinson_regular
    URL https://raw.githubusercontent.com/google/fonts/95f4904fc8bcf26d3420fe315560c96417c6dec7/ofl/atkinsonhyperlegible/AtkinsonHyperlegible-Regular.ttf
    URL_HASH SHA256=7fb917c89019896d0b52ee84b7cbb3304c18cb90b19a62f5e32712bd23e97669
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(font_atkinson_regular)
  set(TOONVAL_FONT_REGULAR_FILE
      ${font_atkinson_regular_SOURCE_DIR}/AtkinsonHyperlegible-Regular.ttf)

  # Icons for the touch UI: the Lucide icon font (ISC; the icons derived from Feather are MIT), from
  # the lucide-font release archive. Merged into the ImGui font atlas, so a control can draw an icon
  # by its codepoint (app/ui/icons.hpp lists the ones used). A binary release must ship
  # Lucide's ISC licence text with it.
  FetchContent_Declare(font_lucide
    URL https://github.com/lucide-icons/lucide/releases/download/1.55.0/lucide-font-1.55.0.zip
    URL_HASH SHA256=f5f44ae0e910259ed1b832e370b3bb96405bb88abef6658b1166fc7053b108c9
    SOURCE_SUBDIR no-add-subdirectory)
  FetchContent_MakeAvailable(font_lucide)
  set(TOONVAL_ICON_FONT_FILE ${font_lucide_SOURCE_DIR}/lucide.ttf)

  # Dear ImGui (MIT), the settings and menu screens. The release tarball has no CMake project, so
  # it is only downloaded here and app/CMakeLists.txt builds its core files plus the SDL3 platform
  # and SDLRenderer3 renderer backends into a static library.
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    SOURCE_SUBDIR no-add-subdirectory
    SYSTEM)
  FetchContent_MakeAvailable(imgui)
endif()

# The app reads live MIDI and the tools list ports and play test patterns.
if(TOONVAL_BUILD_APP OR TOONVAL_BUILD_TOOLS)
  set(LIBREMIDI_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_TESTS OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_BOOST ON CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_JACK ON CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_NETWORK ON CACHE BOOL "" FORCE)
  FetchContent_Declare(libremidi
    URL https://github.com/celtera/libremidi/archive/refs/tags/v5.4.3.tar.gz
    URL_HASH SHA256=a1c369c6aab01c80f1506345d1c099a167b330d1d59159aa7583f7b0b9f966b9
    SYSTEM)
  FetchContent_MakeAvailable(libremidi)
endif()

# TinySoundFont (MIT), the built-in piano's SoundFont player: one header, tsf.h. Upstream has no
# release tags, so this is the archive of a fixed commit (v0.9, 2026-07-19). It is only
# downloaded here; cmake/Synth.cmake compiles the implementation into toonval_tsf.
if(TOONVAL_BUILD_APP OR TOONVAL_BUILD_TESTS)
  FetchContent_Declare(tinysoundfont
    URL https://github.com/schellingb/TinySoundFont/archive/853a0a171759f1ddba0de1442133a75912bbeffa.tar.gz
    URL_HASH SHA256=76c356df524f71f2a34f67fa0cb2cb946036fe7a65d2930a849569c5e742fa57
    SOURCE_SUBDIR no-add-subdirectory
    SYSTEM)
  FetchContent_MakeAvailable(tinysoundfont)
endif()

# Score files (MusicXML, .mxl, .mscz) are read inside core, from bytes the caller hands over.
# pugixml (MIT) parses the XML and miniz (MIT) unzips .mxl and .mscz. Neither release ships a build
# we want: pugixml's is a project with options and miniz's needs a generated export header. So both
# are only downloaded (SOURCE_SUBDIR points at a missing folder) and built here as static libraries
# private to core. Their warnings are not ours to fix: the include folders are SYSTEM and the
# compile flags drop all warnings.
FetchContent_Declare(pugixml
  URL https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.tar.gz
  URL_HASH SHA256=4cee1ca4aad395170f4c7a07824f3bdd41f28316c6e1e1090a1425b278ec0b4b
  SOURCE_SUBDIR no-add-subdirectory
  SYSTEM)
FetchContent_MakeAvailable(pugixml)
add_library(toonval_pugixml STATIC ${pugixml_SOURCE_DIR}/src/pugixml.cpp)
target_include_directories(toonval_pugixml SYSTEM PUBLIC ${pugixml_SOURCE_DIR}/src)
# No exceptions across core's API, no XPath (unused), no file or stream access.
target_compile_definitions(toonval_pugixml PUBLIC
  PUGIXML_NO_EXCEPTIONS PUGIXML_NO_XPATH PUGIXML_NO_STL)

FetchContent_Declare(miniz
  URL https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
  URL_HASH SHA256=98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb
  SOURCE_SUBDIR no-add-subdirectory
  SYSTEM)
FetchContent_MakeAvailable(miniz)
# miniz.h includes a header CMake generates in its own build; an empty export macro is all we need
# for a static library.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz_generated/miniz_export.h
  "#pragma once\n#define MINIZ_EXPORT\n")
add_library(toonval_miniz STATIC
  ${miniz_SOURCE_DIR}/miniz.c ${miniz_SOURCE_DIR}/miniz_tdef.c ${miniz_SOURCE_DIR}/miniz_tinfl.c
  ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(toonval_miniz SYSTEM PUBLIC ${miniz_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/miniz_generated)
# core reads zips from memory only: no stdio, so no file system access, and no writing.
target_compile_definitions(toonval_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_WRITING_APIS)
if(MSVC)
  target_compile_options(toonval_miniz PRIVATE /w)
  target_compile_options(toonval_pugixml PRIVATE /w)
else()
  target_compile_options(toonval_miniz PRIVATE -w)
  target_compile_options(toonval_pugixml PRIVATE -w)
endif()
set_target_properties(toonval_miniz toonval_pugixml PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# KISS FFT (BSD-3-Clause), the FFT behind core's pitch detector. Only kiss_fft.c and kiss_fftr.c are
# built, as a static library private to core, with float samples. The detector only uses
# power-of-two sizes and out-of-place real transforms, where KISS FFT never allocates after
# kiss_fftr_alloc, so a transform is safe on the real-time analysis thread.
FetchContent_Declare(kissfft
  URL https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz
  URL_HASH SHA256=205a8f6a448ef12b091f8ac6a514b5091bb5f6b0b543431ed75f673116cf5cbf
  SOURCE_SUBDIR no-add-subdirectory
  SYSTEM)
FetchContent_MakeAvailable(kissfft)
add_library(toonval_kissfft STATIC ${kissfft_SOURCE_DIR}/kiss_fft.c ${kissfft_SOURCE_DIR}/kiss_fftr.c)
target_include_directories(toonval_kissfft SYSTEM PUBLIC ${kissfft_SOURCE_DIR})
target_compile_definitions(toonval_kissfft PUBLIC kiss_fft_scalar=float)
if(MSVC)
  target_compile_options(toonval_kissfft PRIVATE /w)
else()
  target_compile_options(toonval_kissfft PRIVATE -w)
endif()
set_target_properties(toonval_kissfft PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

if(TOONVAL_BUILD_TESTS)
  FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
    SYSTEM)
  FetchContent_MakeAvailable(doctest)
endif()
