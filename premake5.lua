workspace "dp8-dwmglass"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "dp8-dwmglass"
	location "build/solutions"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

local w8sdk_dir = _MAIN_SCRIPT_DIR .. "/external/w8.1sdk"

common_platform = function()
    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "Speed"
        symbols "Off"

    filter { "system:windows", "configurations:Debug" }
        staticruntime "On"

    filter { "system:windows", "configurations:Release" }
        staticruntime "On"

    filter {}
end

project "symfetch_lib"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/common/symfetch_lib.cpp"
    }

    includedirs {
        "include",
        "include/common"
    }

    common_platform()

project "dp8-dwmglass"
    kind "SharedLib"
    language "C++"
    cppdialect "C++17"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/common/logging.cpp",
        "src/hook/**.cpp",
        "external/minhook/src/buffer.c",
        "external/minhook/src/hook.c",
        "external/minhook/src/trampoline.c",
        "external/minhook/src/hde/hde64.c"
    }

    includedirs {
        "include",
        "include/common",
        "include/hook",
        "external/minhook/include",
        "external/minhook/src",
        "external/minhook/src/hde"
    }

    links {
        "symfetch_lib",
        "d3d11",
        "dxgi",
        "dwmapi",
        "gdi32",
        "user32",
        "advapi32",
        "userenv",
        "psapi",
        "dbghelp",
        "shlwapi",
        "shell32",
        "uxtheme"
    }

    defines {
        "UNICODE",
        "_UNICODE"
    }

    common_platform()

project "injector"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/tools/injector.cpp"
    }

    includedirs {
        "include"
    }

    links {
        "advapi32",
        "userenv",
        "psapi"
    }

    common_platform()

project "symfetch"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/tools/symfetch.cpp"
    }

    includedirs {
        "include",
        "include/common"
    }

    links {
        "symfetch_lib",
        "dbghelp",
        "advapi32",
        "shell32"
    }

    postbuildcommands {
        "{COPYFILE} \"" .. path.getrelative(project().location, path.join(w8sdk_dir, "dbghelp.dll")) .. "\" \"%{cfg.targetdir}/dbghelp.dll\"",
        "{COPYFILE} \"" .. path.getrelative(project().location, path.join(w8sdk_dir, "symsrv.dll")) .. "\" \"%{cfg.targetdir}/symsrv.dll\""
    }

    common_platform()
