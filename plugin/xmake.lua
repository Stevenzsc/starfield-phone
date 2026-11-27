-- PhoneTest SFSE plugin (P0 spike for the hand-held phone mod), built on CommonLibSF (libxse)
includes("lib/commonlibsf")

set_project("PhoneTest")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

target("PhoneTest")
    add_rules("commonlibsf.plugin", {
        name = "PhoneTest",
        author = "Stevenzsc",
        description = "Phone mod P0: perform 1st-person hand-held actions from hotkeys"
    })
    add_files("src/**.cpp")
    add_includedirs("src")
    -- no precompiled header: screen.cpp includes <windows.h>/<d3d12.h> without CommonLibSF
    add_syslinks("d3d12")
