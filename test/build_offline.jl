using vst3sdk_jll
function build_offline(dir; root = dirname(@__DIR__))
    mkpath(dir)
    lilv = AudioPlugins.LV2Host_jll.Lilv_jll
    sdk = joinpath(vst3sdk_jll.artifact_dir, "include", "vst3sdk")
    sdklib = joinpath(vst3sdk_jll.artifact_dir, "lib", "vst3sdk")
    cc, cxx = AudioPlugins._c_compiler(), AudioPlugins._cxx_compiler()
    arch = AudioPlugins._c_arch_flags()
    lv2 = joinpath(dir, "liblv2_host." * Libdl.dlext)
    run(`$cc $arch -std=gnu99 -O2 -fPIC -shared -Wall -Wextra -I$root/csrc/vendor
         -I$(joinpath(lilv.artifact_dir, "include", "lilv-0"))
         $root/csrc/lv2_host.c $(lilv.liblilv_path) -lm -o $lv2`)
    hosting = joinpath(sdk, "public.sdk", "source", "vst", "hosting")
    platform = if Sys.iswindows()
        ["-lavrt", "-lole32", "-luuid", "-luser32", "-lwinmm", "-lshlwapi", "-lshell32", "-Wl,--export-all-symbols", "-static-libstdc++", "-static-libgcc"]
    elseif Sys.isapple()
        ["-framework", "CoreFoundation", "-framework", "CoreAudio", "-framework", "AudioToolbox", "-framework", "Cocoa"]
    else
        ["-ldl", "-lm"]
    end
    module_file = Sys.iswindows() ? "module_win32.cpp" : Sys.isapple() ? "module_mac.mm" : "module_linux.cpp"
    conversion = joinpath(sdk, "public.sdk", "source", "vst", "utility", "stringconvert.cpp")
    common_conversion = joinpath(sdk, "public.sdk", "source", "common", "commonstringconvert.cpp")
    module_object = joinpath(dir, "offline_module.o")
    module_flags = Sys.isapple() ? ["-fobjc-arc"] : String[]
    run(`$cxx $arch -std=c++17 -O2 -fPIC -DRELEASE=1 -I$sdk $module_flags
         -c $hosting/$module_file -o $module_object`)
    vst3 = joinpath(dir, "libvst3_host." * Libdl.dlext)
    run(`$cxx $arch -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -DRELEASE=1 -I$sdk
         $root/csrc/vst3_host.cpp $hosting/plugprovider.cpp $conversion $common_conversion $module_object
         -L$sdklib -lsdk_hosting -lsdk_common -lsdk -lbase -lpluginterfaces
         -pthread $platform -o $vst3`)
    return lv2, vst3
end

function build_parameter_bundles(dir, sdk_paths)
    root = dirname(@__DIR__)
    cc, cxx = AudioPlugins._c_compiler(), AudioPlugins._cxx_compiler()
    arch = AudioPlugins._c_arch_flags()
    lvdir = joinpath(dir, "lv2")
    mkpath(lvdir)
    cp(joinpath(lv2_test_bundle(), "ap_test.lv2"), joinpath(lvdir, "ap_test.lv2"))
    bundle = joinpath(lvdir, "ap_test.lv2")
    binary = only(filter(p -> endswith(p, "." * Libdl.dlext), readdir(bundle; join = true)))
    run(`$cc $arch -O2 -fPIC -shared -DAP_TEST_EXTRA_PARAMS $root/test/plugins/ap_test_lv2.c -o $binary`)
    ttl = joinpath(bundle, "ap_test.ttl")
    text = read(ttl, String)
    ports = join(
        [
            "[ a lv2:InputPort, lv2:ControlPort ; lv2:index $i ; lv2:symbol \"extra$i\" ; lv2:name \"Extra$i\" ; lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 4 ]" for i in 3:9
        ], ",\n"
    )
    text = replace(text, "] ." => "],\n$ports ."; count = 1)
    write(ttl, text)

    sdk, lib = sdk_paths
    vst = joinpath(dir, "params.vst3")
    inner = joinpath(vst, "Contents", AudioPlugins._vst3_module_dir())
    mkpath(inner)
    entry, name, platform = if Sys.iswindows()
        ("dllmain.cpp", "params.vst3", ["-lole32", "-static-libstdc++", "-static-libgcc"])
    elseif Sys.isapple()
        ("macmain.cpp", "params", ["-framework", "CoreFoundation"])
    else
        ("linuxmain.cpp", "params.so", String[])
    end
    output = joinpath(inner, name)
    run(`$cxx $arch -std=c++17 -O2 -fPIC -shared -DRELEASE=1 -DAP_TEST_EXTRA_PARAMS -I$sdk
         $root/test/plugins/ap_test_vst3.cpp $sdk/public.sdk/source/vst/vstsinglecomponenteffect.cpp
         $sdk/public.sdk/source/main/$entry -L$lib -lsdk -lsdk_common -lbase -lpluginterfaces -pthread $platform -o $output`)
    return lv2_default_path(lvdir), vst
end
