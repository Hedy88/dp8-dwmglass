workspace "dp8-dwmglass"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "dp8-dwmglass"
	location "build/solutions"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

project "dp8-dwmglass"
    kind "SharedLib"
    language "C++"
    cppdialect "C++17"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/**.cpp",
        "include/**.h",
        "external/minhook/src/buffer.c",
        "external/minhook/src/hook.c",
        "external/minhook/src/trampoline.c",
        "external/minhook/src/hde/hde64.c"
    }

    removefiles {
        "src/injector.cpp",
        "src/symfetch.cpp"
    }

    includedirs {
        "include",
        "external/minhook/include",
        "external/minhook/src",
        "external/minhook/src/hde"
    }

    links {
        "d3d11",
        "dxgi",
        "d3dcompiler",
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

project "injector"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/injector.cpp"
    }

    includedirs {
        "include"
    }

    links {
        "advapi32",
        "userenv",
        "psapi"
    }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "Speed"

	filter { "system:windows", "configurations:Debug" }
		staticruntime "On"
	
	filter { "system:windows", "configurations:Release" }
		staticruntime "On"

project "symfetch"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"

    targetdir ("build/bin/" .. outputdir)
    objdir ("build/obj/" .. outputdir .. "/%{prj.name}")

    files {
        "src/symfetch.cpp"
    }

    links {
        "dbghelp",
        "shlwapi",
        "shell32"
    }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "Speed"

	filter { "system:windows", "configurations:Debug" }
		staticruntime "On"
	
	filter { "system:windows", "configurations:Release" }
		staticruntime "On"