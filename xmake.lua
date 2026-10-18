-- include subprojects
includes("lib/commonlibnv")

-- set project constants
set_project("commonlibnv-template")
set_version("0.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- config options
set_config("commonlib_toml", true)

-- define targets
target("commonlibnv-template")
    add_rules("commonlibnv.plugin", {
        name = "commonlibnv-template",
        author = "libxse",
        description = "NVSE plugin template using CommonLibNV"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h", "include/**.h")
    add_includedirs("src", "include")
    set_pcxxheader("src/pch.h")
