use std::{
    env,
    fs,
    path::{Path, PathBuf},
    process::Command,
};

fn run(command: &mut Command, label: &str) {
    let status = command.status().unwrap_or_else(|error| {
        panic!("Could not start {label}: {error}. Install CMake and Visual Studio C++ Build Tools.")
    });
    if !status.success() {
        panic!("{label} failed with status {status}");
    }
}

fn copy_dir(source: &Path, destination: &Path) {
    fs::create_dir_all(destination).expect("create output directory");
    for entry in fs::read_dir(source).expect("read source directory") {
        let entry = entry.expect("read directory entry");
        let source_path = entry.path();
        let target_path = destination.join(entry.file_name());
        if source_path.is_dir() {
            copy_dir(&source_path, &target_path);
        } else {
            fs::copy(&source_path, &target_path).expect("copy runtime asset");
        }
    }
}

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os != "windows" {
        println!("cargo:warning=End-scaling currently builds only on Windows.");
        return;
    }

    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("manifest dir"));
    let root = manifest.parent().expect("repository root");
    let native = root.join("native");
    let build_dir = root.join("build").join("native");

    run(
        Command::new("cmake")
            .arg("-S").arg(&native)
            .arg("-B").arg(&build_dir)
            .arg("-DBUILD_TESTING=ON"),
        "CMake configure",
    );

    run(
        Command::new("cmake")
            .arg("--build").arg(&build_dir)
            .arg("--config").arg("Release")
            .arg("--target").arg("end_scaling_core"),
        "C++ D3D11 core build",
    );

    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-search=native={}", build_dir.join("Release").display());
    println!("cargo:rustc-link-lib=static=end_scaling_core");
    println!("cargo:rustc-link-lib=dylib=d3d11");
    println!("cargo:rustc-link-lib=dylib=dxgi");
    println!("cargo:rustc-link-lib=dylib=d3dcompiler");
    println!("cargo:rustc-link-lib=dylib=windowsapp");

    // Keep runtime shader lookup independent of the current working directory.
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR"));
    let profile_dir = out_dir.ancestors().nth(3).expect("Cargo profile directory");
    copy_dir(&native.join("src").join("shaders"), &profile_dir.join("shaders"));
    copy_dir(
        &native.join("third_party").join("amd_fsr1"),
        &profile_dir.join("third_party").join("amd_fsr1"),
    );

    println!("cargo:rerun-if-changed={}", native.display());
}
