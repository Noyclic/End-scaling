#![cfg_attr(windows, windows_subsystem = "windows")]

#[cfg(windows)]
mod app {
    use std::{
        env, ffi::OsStr, fs, os::windows::ffi::OsStrExt, path::PathBuf,
        ptr::{null, null_mut},
    };

    use windows_sys::Win32::{
        Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM},
        Graphics::Gdi::{GetStockObject, HBRUSH, WHITE_BRUSH},
        System::LibraryLoader::GetModuleHandleW,
        UI::WindowsAndMessaging::{
            CreateWindowExW, DefWindowProcW, DispatchMessageW, GetMessageW, GetSystemMetrics,
            PostQuitMessage, RegisterClassW, SendMessageW, SetWindowTextW, ShowWindow,
            TranslateMessage, CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, MSG,
            SW_SHOW, WNDCLASSW, WS_CAPTION, WS_CHILD, WS_MINIMIZEBOX, WS_OVERLAPPED,
            WS_SYSMENU, WS_TABSTOP, WS_VISIBLE, WS_VSCROLL, WM_COMMAND, WM_CREATE,
            WM_DESTROY, SM_CXSCREEN, SM_CYSCREEN,
        },
    };

    const CB_ADDSTRING: u32 = 0x0143;
    const CB_GETCURSEL: u32 = 0x0147;
    const CB_SETCURSEL: u32 = 0x014E;
    const CBN_SELCHANGE: u16 = 1;
    const CBS_DROPDOWNLIST: u32 = 0x0003;
    const BS_DEFPUSHBUTTON: u32 = 0x0001;
    const BUTTON_ID: usize = 1001;

    static mut INSTANCE: HINSTANCE = null_mut();
    static mut PROFILE_COMBO: HWND = null_mut();
    static mut RESOLUTION_LABEL: HWND = null_mut();
    static mut GPU_LABEL: HWND = null_mut();
    static mut STATUS_LABEL: HWND = null_mut();
    static mut SELECTED_PROFILE: i32 = 1;

    #[link(name = "end_scaling_core", kind = "static")]
    unsafe extern "C" {
        fn es_core_version() -> u32;
        fn es_probe_d3d11_gpu(
            shader_path: *const u16,
            gpu_name: *mut u16,
            gpu_name_capacity: i32,
            error_text: *mut u16,
            error_capacity: i32,
            feature_level: *mut u32,
        ) -> i32;
    }

    fn wide(value: &str) -> Vec<u16> {
        OsStr::new(value).encode_wide().chain(Some(0)).collect()
    }

    fn from_wide(buffer: &[u16]) -> String {
        let end = buffer.iter().position(|value| *value == 0).unwrap_or(buffer.len());
        String::from_utf16_lossy(&buffer[..end])
    }

    fn profile_name(index: i32) -> &'static str {
        match index {
            0 => "Quality",
            2 => "Performance",
            _ => "Balanced",
        }
    }

    fn profile_scale(index: i32) -> f32 {
        match index {
            0 => 0.77,
            2 => 0.50,
            _ => 0.67,
        }
    }

    unsafe fn selected_profile() -> i32 {
        let current = SendMessageW(PROFILE_COMBO, CB_GETCURSEL, 0, 0) as i32;
        if current < 0 { SELECTED_PROFILE } else { current }
    }

    unsafe fn update_resolution_label() {
        let index = selected_profile();
        let screen_width = GetSystemMetrics(SM_CXSCREEN).max(1) as f32;
        let screen_height = GetSystemMetrics(SM_CYSCREEN).max(1) as f32;
        let scale = profile_scale(index);
        let width = ((screen_width * scale) as u32 / 8) * 8;
        let height = ((screen_height * scale) as u32 / 8) * 8;
        let label = wide(&format!(
            "Рекомендуемое разрешение игры: {width} × {height}  (экран {screen_width:.0} × {screen_height:.0})"
        ));
        SetWindowTextW(RESOLUTION_LABEL, label.as_ptr());
    }

    fn settings_path() -> Option<PathBuf> {
        let app_data = env::var_os("APPDATA")?;
        Some(PathBuf::from(app_data).join("EndScaling").join("settings.ini"))
    }

    fn load_profile() -> i32 {
        let Some(path) = settings_path() else { return 1; };
        let Ok(contents) = fs::read_to_string(path) else { return 1; };
        for line in contents.lines() {
            if let Some(value) = line.strip_prefix("profile=") {
                return match value.trim() {
                    "Quality" => 0,
                    "Performance" => 2,
                    _ => 1,
                };
            }
        }
        1
    }

    unsafe fn save_profile() {
        let index = selected_profile();
        let Some(path) = settings_path() else {
            SetWindowTextW(STATUS_LABEL, wide("Не удалось найти папку APPDATA.").as_ptr());
            return;
        };
        let Some(parent) = path.parent() else { return; };
        if let Err(error) = fs::create_dir_all(parent) {
            SetWindowTextW(STATUS_LABEL, wide(&format!("Ошибка папки настроек: {error}")).as_ptr());
            return;
        }

        let contents = format!(
            "profile={}\nscale_factor={:.2}\n",
            profile_name(index),
            profile_scale(index)
        );
        match fs::write(&path, contents) {
            Ok(()) => {
                SELECTED_PROFILE = index;
                SetWindowTextW(STATUS_LABEL, wide("Профиль сохранён. Разрешение игры нужно изменить вручную.").as_ptr());
                update_resolution_label();
            }
            Err(error) => {
                SetWindowTextW(STATUS_LABEL, wide(&format!("Не удалось сохранить настройки: {error}")).as_ptr());
            }
        }
    }

    unsafe fn probe_gpu() {
        let exe = match env::current_exe() {
            Ok(value) => value,
            Err(error) => {
                SetWindowTextW(GPU_LABEL, wide(&format!("D3D11: путь приложения недоступен: {error}")).as_ptr());
                return;
            }
        };
        let Some(parent) = exe.parent() else { return; };
        let shader_path = parent.join("shaders").join("fsr1.hlsl");
        let shader_wide = shader_path.as_os_str().encode_wide().chain(Some(0)).collect::<Vec<_>>();

        let mut gpu_name = [0u16; 128];
        let mut error_text = [0u16; 512];
        let mut feature_level = 0u32;
        let ok = es_probe_d3d11_gpu(
            shader_wide.as_ptr(),
            gpu_name.as_mut_ptr(),
            gpu_name.len() as i32,
            error_text.as_mut_ptr(),
            error_text.len() as i32,
            &mut feature_level,
        );

        let status = if ok == 1 {
            let level = match feature_level {
                0xB000 => "11.0",
                0xA100 => "10.1",
                0xA000 => "10.0",
                _ => "unknown",
            };
            format!("GPU: {}  •  Direct3D {level}  •  FSR shaders готовы", from_wide(&gpu_name))
        } else {
            format!("Графическое ядро не готово: {}", from_wide(&error_text))
        };
        SetWindowTextW(GPU_LABEL, wide(&status).as_ptr());
    }

    unsafe fn create_label(parent: HWND, text: &str, x: i32, y: i32, width: i32, height: i32) -> HWND {
        CreateWindowExW(
            0, wide("STATIC").as_ptr(), wide(text).as_ptr(),
            WS_CHILD | WS_VISIBLE, x, y, width, height,
            parent, null_mut(), INSTANCE, null(),
        )
    }

    unsafe fn create_controls(parent: HWND) {
        create_label(parent, "END SCALING", 24, 20, 450, 34);
        GPU_LABEL = create_label(parent, "Проверка графического адаптера...", 24, 62, 465, 44);
        create_label(parent, "Профиль масштабирования", 24, 128, 300, 22);

        PROFILE_COMBO = CreateWindowExW(
            0, wide("COMBOBOX").as_ptr(), wide("").as_ptr(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            24, 154, 300, 150, parent, null_mut(), INSTANCE, null(),
        );

        for value in ["Quality — качество", "Balanced — баланс", "Performance — минимум нагрузки"] {
            let item = wide(value);
            SendMessageW(PROFILE_COMBO, CB_ADDSTRING, 0, item.as_ptr() as LPARAM);
        }
        SendMessageW(PROFILE_COMBO, CB_SETCURSEL, SELECTED_PROFILE as WPARAM, 0);

        RESOLUTION_LABEL = create_label(parent, "", 24, 194, 465, 34);
        let button_text = wide("Сохранить профиль");
        CreateWindowExW(
            0, wide("BUTTON").as_ptr(), button_text.as_ptr(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            24, 236, 180, 34, parent, BUTTON_ID as _, INSTANCE, null(),
        );
        STATUS_LABEL = create_label(parent, "Настройки ещё не сохранены.", 24, 280, 465, 34);

        probe_gpu();
        update_resolution_label();
    }

    unsafe extern "system" fn window_proc(
        hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM,
    ) -> LRESULT {
        match message {
            WM_CREATE => {
                create_controls(hwnd);
                0
            }
            WM_COMMAND => {
                let control_id = (wparam & 0xffff) as usize;
                let notification = ((wparam >> 16) & 0xffff) as u16;
                if control_id == BUTTON_ID {
                    save_profile();
                    return 0;
                }
                if PROFILE_COMBO != null_mut() && notification == CBN_SELCHANGE {
                    update_resolution_label();
                    return 0;
                }
                DefWindowProcW(hwnd, message, wparam, lparam)
            }
            WM_DESTROY => {
                PostQuitMessage(0);
                0
            }
            _ => DefWindowProcW(hwnd, message, wparam, lparam),
        }
    }

    pub fn run() {
        unsafe {
            SELECTED_PROFILE = load_profile();
            INSTANCE = GetModuleHandleW(null());
            let class_name = wide("EndScalingWindow");
            let title = wide("End Scaling");

            let class = WNDCLASSW {
                style: CS_HREDRAW | CS_VREDRAW,
                lpfnWndProc: Some(window_proc),
                cbClsExtra: 0,
                cbWndExtra: 0,
                hInstance: INSTANCE,
                hIcon: null_mut(),
                hCursor: null_mut(),
                hbrBackground: GetStockObject(WHITE_BRUSH) as HBRUSH,
                lpszMenuName: null(),
                lpszClassName: class_name.as_ptr(),
            };
            RegisterClassW(&class);

            let hwnd = CreateWindowExW(
                0, class_name.as_ptr(), title.as_ptr(),
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                CW_USEDEFAULT, CW_USEDEFAULT, 540, 370,
                null_mut(), null_mut(), INSTANCE, null(),
            );
            if hwnd.is_null() {
                return;
            }

            ShowWindow(hwnd, SW_SHOW);

            let mut message: MSG = std::mem::zeroed();
            while GetMessageW(&mut message, null_mut(), 0, 0) > 0 {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            let _ = es_core_version();
        }
    }
}

#[cfg(windows)]
fn main() {
    app::run();
}

#[cfg(not(windows))]
fn main() {
    eprintln!("End-scaling currently targets Windows 10/11.");
}
