# Source-only build using the same compilers as the offline fixture tests.
# JLLs supply build dependencies; installed host libraries are never replaced.
using vst3sdk_jll
function build_live(dir)
    mkpath(dir)
    root = dirname(@__DIR__)
    lilv = AudioPlugins.LV2Host_jll.Lilv_jll
    sdk = joinpath(vst3sdk_jll.artifact_dir, "include", "vst3sdk")
    sdklib = joinpath(vst3sdk_jll.artifact_dir, "lib", "vst3sdk")
    cc, cxx = AudioPlugins._c_compiler(), AudioPlugins._cxx_compiler()
    arch = AudioPlugins._c_arch_flags()
    objects = String[]
    for name in ("clap_live", "live_device", "lv2_live")
        object = joinpath(dir, name * ".o")
        run(`$cc $arch -std=c11 -O2 -fPIC -DAP_LIVE_WITH_DEVICE -I$root/csrc/vendor
             -I$(joinpath(lilv.artifact_dir, "include", "lilv-0"))
             -c $root/csrc/$name.c -o $object`)
        push!(objects, object)
    end
    hosting = joinpath(sdk, "public.sdk", "source", "vst", "hosting")
    platform = if Sys.iswindows()
        ["-lavrt", "-lole32", "-luuid", "-luser32", "-lwinmm", "-lshlwapi", "-lshell32", "-Wl,--export-all-symbols", "-static-libstdc++", "-static-libgcc"]
    elseif Sys.isapple()
        ["-framework", "CoreFoundation", "-framework", "CoreAudio", "-framework", "AudioToolbox", "-framework", "Cocoa"]
    else
        ["-ldl", "-lm"]
    end
    module_file = Sys.iswindows() ? "module_win32.cpp" : Sys.isapple() ? "module_mac.mm" : "module_linux.cpp"
    # Build the SDK conversion helper with this compiler. New MinGW releases
    # changed mbstate_t, so its prebuilt codecvt object is not ABI-compatible.
    conversion = joinpath(sdk, "public.sdk", "source", "vst", "utility", "stringconvert.cpp")
    common_conversion = joinpath(sdk, "public.sdk", "source", "common", "commonstringconvert.cpp")
    module_object = joinpath(dir, "module.o")
    module_flags = Sys.isapple() ? ["-fobjc-arc"] : String[]
    run(`$cxx $arch -std=c++17 -O2 -fPIC -DRELEASE=1 -I$sdk $module_flags
         -c $hosting/$module_file -o $module_object`)
    push!(objects, module_object)
    library = joinpath(dir, "libaudioplugins_live." * Libdl.dlext)
    run(`$cxx $arch -std=c++17 -O2 -fPIC -shared -DRELEASE=1 -I$sdk
         $root/csrc/vst3_live.cpp $hosting/plugprovider.cpp $conversion $common_conversion $objects
         $(lilv.liblilv_path) -L$sdklib -lsdk_hosting -lsdk_common -lsdk -lbase -lpluginterfaces
         -pthread $platform -o $library`)
    return library, (sdk, sdklib)
end

function build_vst_live_fixture(dir, sdk_paths)
    sdk, lib = sdk_paths
    cxx = AudioPlugins._cxx_compiler()
    bundle = joinpath(dir, "live.vst3")
    inner = joinpath(bundle, "Contents", AudioPlugins._vst3_module_dir())
    mkpath(inner)
    entry, name, platform = if Sys.iswindows()
        ("dllmain.cpp", "live.vst3", ["-lole32", "-static-libstdc++", "-static-libgcc"])
    elseif Sys.isapple()
        ("macmain.cpp", "live", ["-framework", "CoreFoundation"])
    else
        ("linuxmain.cpp", "live.so", String[])
    end
    output = joinpath(inner, name)
    source = joinpath(@__DIR__, "plugins", "ap_test_vst3_live.cpp")
    run(`$cxx $(AudioPlugins._c_arch_flags()) -std=c++17 -O2 -fPIC -shared -DRELEASE=1 -I$sdk
         $source $sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp
         $sdk/public.sdk/source/main/$entry -L$lib -lsdk -lsdk_common -lbase -lpluginterfaces -pthread $platform -o $output`)
    return bundle
end
