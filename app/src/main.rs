#![cfg_attr(windows, windows_subsystem = "windows")]

#[cfg(windows)]
mod app {
    use std::{
        env, ffi::{c_void, OsStr}, fs, os::windows::ffi::OsStrExt,
        path::PathBuf, ptr::{null, null_mut},
    };

    use windows_sys::Win32::{
        UI::Input::KeyboardAndMouse::{RegisterHotKey, UnregisterHotKey},
        Foundation::{BOOL, HINSTANCE, HWND, LPARAM, LRESULT, RECT, WPARAM},
        Graphics::Gdi::{GetStockObject, HBRUSH, BLACK_BRUSH, WHITE_BRUSH},
        System::{LibraryLoader::GetModuleHandleW, Threading::GetCurrentProcessId},
        UI::WindowsAndMessaging::{
            CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, EnumWindows,
            GetClientRect, GetMessageW, GetSystemMetrics, GetWindowTextLengthW, GetWindowTextW,
            GetWindowThreadProcessId, GetWindowRect, GetForegroundWindow, IsIconic, IsWindow,
            IsWindowVisible, KillTimer, PostQuitMessage, RegisterClassW, SendMessageW,
            SetTimer, SetWindowPos, SetWindowTextW, ShowWindow,
            TranslateMessage, CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, HWND_TOPMOST, MSG,
            SW_HIDE, SW_SHOW, SWP_NOACTIVATE, SWP_SHOWWINDOW, WNDCLASSW,
            WS_CAPTION, WS_CHILD, WS_MINIMIZEBOX, WS_MAXIMIZEBOX, WS_OVERLAPPED,
            WS_SYSMENU, WS_TABSTOP, WS_THICKFRAME, WS_VISIBLE, WS_VSCROLL,
            WM_CLOSE, WM_COMMAND, WM_CREATE, WM_DESTROY, WM_HOTKEY, WM_TIMER, SM_CXSCREEN, SM_CYSCREEN,
        },
    };

    const CB_ADDSTRING: u32 = 0x0143;
    const CB_GETCURSEL: u32 = 0x0147;
    const CB_SETCURSEL: u32 = 0x014E;
    const CB_GETITEMDATA: u32 = 0x0150;
    const CB_SETITEMDATA: u32 = 0x0151;
    const CB_RESETCONTENT: u32 = 0x014B;
    const CBN_SELCHANGE: u16 = 1;
    const CBS_DROPDOWNLIST: u32 = 0x0003;
    const BS_DEFPUSHBUTTON: u32 = 0x0001;

    const ID_PROFILE: usize = 1001;
    const ID_SAVE: usize = 1002;
    const ID_WINDOW_LIST: usize = 1003;
    const ID_REFRESH: usize = 1004;
    const ID_START: usize = 1005;
    const ID_STOP: usize = 1006;
    const ID_OVERLAY: usize = 1007;
    const HOTKEY_EMERGENCY_STOP: i32 = 1;
    const MOD_NOREPEAT: u32 = 0x4000;
    const MOD_CONTROL_ALT: u32 = 0x0003 | MOD_NOREPEAT;
    const VK_F9: u32 = 0x78;
    const WS_POPUP: u32 = 0x80000000;

    static mut INSTANCE: HINSTANCE = null_mut();
    static mut MAIN_HWND: HWND = null_mut();
    static mut PREVIEW_HWND: HWND = null_mut();
    static mut PROFILE_COMBO: HWND = null_mut();
    static mut WINDOW_COMBO: HWND = null_mut();
    static mut RESOLUTION_LABEL: HWND = null_mut();
    static mut GPU_LABEL: HWND = null_mut();
    static mut STATUS_LABEL: HWND = null_mut();
    static mut SELECTED_PROFILE: i32 = 1;
    static mut CAPTURE_HANDLE: *mut c_void = null_mut();
    static mut CAPTURE_SOURCE_HWND: HWND = null_mut();
    static mut OVERLAY_MODE: bool = false;
    static mut HOTKEY_REGISTERED: bool = false;

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

        fn es_capture_start(
            source_window: HWND,
            output_window: HWND,
            mode: u32,
            shader_path: *const u16,
            error_text: *mut u16,
            error_capacity: i32,
        ) -> *mut c_void;

        fn es_capture_stop(handle: *mut c_void);
        fn es_capture_get_stats(
            handle: *mut c_void,
            fps: *mut f64,
            submit_ms: *mut f64,
            gpu_ms: *mut f64,
            frames: *mut u64,
        ) -> i32;
    }

    struct WindowEntry {
        hwnd: HWND,
        title: String,
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

    unsafe fn selected_source_window() -> HWND {
        if WINDOW_COMBO.is_null() {
            return null_mut();
        }
        let index = SendMessageW(WINDOW_COMBO, CB_GETCURSEL, 0, 0);
        if index < 0 {
            return null_mut();
        }
        SendMessageW(WINDOW_COMBO, CB_GETITEMDATA, index as WPARAM, 0) as HWND
    }

    unsafe fn update_resolution_label() {
        let source = selected_source_window();
        let mut rect = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        let (width, height) = if !source.is_null() && IsWindow(source) != 0 &&
            GetClientRect(source, &mut rect) != 0 &&
            rect.right > rect.left && rect.bottom > rect.top {
            ((rect.right - rect.left) as u32, (rect.bottom - rect.top) as u32)
        } else {
            (
                GetSystemMetrics(SM_CXSCREEN).max(1) as u32,
                GetSystemMetrics(SM_CYSCREEN).max(1) as u32,
            )
        };
        let scale = profile_scale(selected_profile());
        let scaled_width = (((width as f32 * scale) as u32) / 8) * 8;
        let scaled_height = (((height as f32 * scale) as u32) / 8) * 8;
        let label = wide(&format!(
            "Рекомендуемое разрешение игры: {scaled_width} × {scaled_height}  (исходное: {width} × {height})"
        ));
        SetWindowTextW(RESOLUTION_LABEL, label.as_ptr());
    }

    fn settings_path() -> Option<PathBuf> {
        Some(PathBuf::from(env::var_os("APPDATA")?)
            .join("EndScaling")
            .join("settings.ini"))
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

    unsafe fn set_status(message: &str) {
        if !STATUS_LABEL.is_null() {
            SetWindowTextW(STATUS_LABEL, wide(message).as_ptr());
        }
    }

    unsafe fn save_profile() {
        let index = selected_profile();
        let Some(path) = settings_path() else {
            set_status("Не удалось найти папку APPDATA.");
            return;
        };
        let Some(parent) = path.parent() else { return; };
        if let Err(error) = fs::create_dir_all(parent) {
            set_status(&format!("Ошибка папки настроек: {error}"));
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
                set_status("Профиль сохранён. Разрешение игры нужно выставить вручную.");
                update_resolution_label();
            }
            Err(error) => set_status(&format!("Не удалось сохранить настройки: {error}")),
        }
    }

    unsafe fn current_shader_path() -> Option<Vec<u16>> {
        let exe = env::current_exe().ok()?;
        let path = exe.parent()?.join("shaders").join("fsr1.hlsl");
        Some(path.as_os_str().encode_wide().chain(Some(0)).collect())
    }

    unsafe fn probe_gpu() {
        let Some(shader_path) = current_shader_path() else {
            SetWindowTextW(GPU_LABEL, wide("Не удалось определить путь к шейдерам.").as_ptr());
            return;
        };

        let mut gpu_name = [0u16; 128];
        let mut error_text = [0u16; 512];
        let mut feature_level = 0u32;
        let ok = es_probe_d3d11_gpu(
            shader_path.as_ptr(),
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
            format!("GPU: {}  •  Direct3D {level}  •  FSR готов", from_wide(&gpu_name))
        } else {
            format!("Графическое ядро не готово: {}", from_wide(&error_text))
        };
        SetWindowTextW(GPU_LABEL, wide(&status).as_ptr());
    }

    unsafe fn create_label(parent: HWND, text: &str, x: i32, y: i32, w: i32, h: i32) -> HWND {
        CreateWindowExW(
            0, wide("STATIC").as_ptr(), wide(text).as_ptr(),
            WS_CHILD | WS_VISIBLE, x, y, w, h,
            parent, null_mut(), INSTANCE, null(),
        )
    }

    unsafe fn create_button(parent: HWND, text: &str, id: usize,
                            x: i32, y: i32, w: i32, h: i32, primary: bool) -> HWND {
        let style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            if primary { BS_DEFPUSHBUTTON } else { 0 };
        CreateWindowExW(
            0, wide("BUTTON").as_ptr(), wide(text).as_ptr(),
            style, x, y, w, h, parent, id as _, INSTANCE, null(),
        )
    }

    unsafe extern "system" fn enum_window_callback(hwnd: HWND, lparam: LPARAM) -> BOOL {
        if hwnd.is_null() || IsWindowVisible(hwnd) == 0 || hwnd == MAIN_HWND {
            return 1;
        }

        let mut process_id = 0u32;
        GetWindowThreadProcessId(hwnd, &mut process_id);
        if process_id == GetCurrentProcessId() {
            return 1;
        }

        let length = GetWindowTextLengthW(hwnd);
        if length <= 0 || length > 1024 {
            return 1;
        }
        let mut title = vec![0u16; length as usize + 1];
        let copied = GetWindowTextW(hwnd, title.as_mut_ptr(), title.len() as i32);
        if copied <= 0 {
            return 1;
        }

        title.truncate(copied as usize);
        let title = String::from_utf16_lossy(&title);
        if title.trim().is_empty() {
            return 1;
        }

        let entries = &mut *(lparam as *mut Vec<WindowEntry>);
        entries.push(WindowEntry { hwnd, title });
        1
    }

    unsafe fn refresh_window_list() {
        if WINDOW_COMBO.is_null() {
            return;
        }
        SendMessageW(WINDOW_COMBO, CB_RESETCONTENT, 0, 0);
        let mut windows = Vec::<WindowEntry>::new();
        EnumWindows(Some(enum_window_callback), &mut windows as *mut _ as LPARAM);

        for entry in windows {
            let display = wide(&entry.title);
            let index = SendMessageW(WINDOW_COMBO, CB_ADDSTRING, 0, display.as_ptr() as LPARAM);
            if index >= 0 {
                SendMessageW(
                    WINDOW_COMBO, CB_SETITEMDATA,
                    index as WPARAM, entry.hwnd as LPARAM,
                );
            }
        }

        if SendMessageW(WINDOW_COMBO, CB_GETCURSEL, 0, 0) < 0 {
            SendMessageW(WINDOW_COMBO, CB_SETCURSEL, 0, 0);
        }
        update_resolution_label();
        set_status("Список окон обновлён. Выбери игру или приложение.");
    }

    unsafe fn stop_capture_only() {
        if !MAIN_HWND.is_null() {
            KillTimer(MAIN_HWND, 1);
            KillTimer(MAIN_HWND, 2);
        }
        OVERLAY_MODE = false;
        CAPTURE_SOURCE_HWND = null_mut();
        if !CAPTURE_HANDLE.is_null() {
            es_capture_stop(CAPTURE_HANDLE);
            CAPTURE_HANDLE = null_mut();
        }
    }

    unsafe fn update_capture_stats() {
        if CAPTURE_HANDLE.is_null() {
            return;
        }
        let mut preview_fps = 0.0f64;
        let mut submit_ms = 0.0f64;
        let mut gpu_ms = 0.0f64;
        let mut frames = 0u64;
        if es_capture_get_stats(
            CAPTURE_HANDLE,
            &mut preview_fps,
            &mut submit_ms,
            &mut gpu_ms,
            &mut frames,
        ) != 0 {
            let label = if OVERLAY_MODE {
                format!("Оверлей: {preview_fps:.1} FPS  •  GPU scale: {gpu_ms:.2} ms  •  CPU submit: {submit_ms:.2} ms  •  кадров: {frames}")
            } else {
                format!("Предпросмотр: {preview_fps:.1} FPS  •  GPU scale: {gpu_ms:.2} ms  •  CPU submit: {submit_ms:.2} ms  •  кадров: {frames}")
            };
            set_status(&label);
        }
    }

    unsafe extern "system" fn preview_window_proc(
        hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM,
    ) -> LRESULT {
        match message {
            WM_CLOSE => {
                stop_capture_only();
                DestroyWindow(hwnd);
                0
            }
            WM_DESTROY => {
                if PREVIEW_HWND == hwnd {
                    PREVIEW_HWND = null_mut();
                }
                0
            }
            _ => DefWindowProcW(hwnd, message, wparam, lparam),
        }
    }

    unsafe fn update_overlay_position() {
        if !OVERLAY_MODE || PREVIEW_HWND.is_null() || CAPTURE_SOURCE_HWND.is_null() ||
            IsWindow(PREVIEW_HWND) == 0 || IsWindow(CAPTURE_SOURCE_HWND) == 0 {
            return;
        }

        // Keep the overlay only over the active source window. Alt+Tab hides it.
        let should_show = IsWindowVisible(CAPTURE_SOURCE_HWND) != 0 &&
            IsIconic(CAPTURE_SOURCE_HWND) == 0 &&
            GetForegroundWindow() == CAPTURE_SOURCE_HWND;
        if !should_show {
            if IsWindowVisible(PREVIEW_HWND) != 0 {
                ShowWindow(PREVIEW_HWND, SW_HIDE);
            }
            return;
        }

        let mut rect = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        if GetWindowRect(CAPTURE_SOURCE_HWND, &mut rect) == 0 {
            ShowWindow(PREVIEW_HWND, SW_HIDE);
            return;
        }
        let width = rect.right - rect.left;
        let height = rect.bottom - rect.top;
        if width <= 0 || height <= 0 {
            ShowWindow(PREVIEW_HWND, SW_HIDE);
            return;
        }

        let mut current = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        let already_aligned = IsWindowVisible(PREVIEW_HWND) != 0 &&
            GetWindowRect(PREVIEW_HWND, &mut current) != 0 &&
            current.left == rect.left && current.top == rect.top &&
            current.right == rect.right && current.bottom == rect.bottom;
        if !already_aligned {
            SetWindowPos(
                PREVIEW_HWND,
                HWND_TOPMOST,
                rect.left,
                rect.top,
                width,
                height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW,
            );
        }
    }

    unsafe fn create_output_window(overlay: bool, source: HWND) -> HWND {
        let class_name = wide("EndScalingPreviewWindow");
        let class = WNDCLASSW {
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(preview_window_proc),
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: INSTANCE,
            hIcon: null_mut(),
            hCursor: null_mut(),
            hbrBackground: GetStockObject(BLACK_BRUSH) as HBRUSH,
            lpszMenuName: null(),
            lpszClassName: class_name.as_ptr(),
        };
        RegisterClassW(&class);

        let title = wide(if overlay {
            "End-scaling — игровой оверлей"
        } else {
            "End-scaling — предпросмотр"
        });
        let mut x = CW_USEDEFAULT;
        let mut y = CW_USEDEFAULT;
        let mut width = 960;
        let mut height = 540;
        if overlay {
            let mut rect = RECT { left: 0, top: 0, right: 0, bottom: 0 };
            if GetWindowRect(source, &mut rect) == 0 {
                return null_mut();
            }
            x = rect.left;
            y = rect.top;
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
            if width <= 0 || height <= 0 {
                return null_mut();
            }
        }

        // Keep the flip-model HWND path (no layered redirection surface).
        // WS_EX_TRANSPARENT is used for click-through on the Win10+ desktop path.
        // F9 is registered globally as an emergency stop.
        let extended_style = if overlay {
            0x00000008 | 0x00000020 | 0x00000080 | 0x08000000
        } else {
            0
        };
        let style = if overlay {
            WS_POPUP // deliberately hidden until the source game is foreground
        } else {
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VISIBLE
        };

        let hwnd = CreateWindowExW(
            extended_style, class_name.as_ptr(), title.as_ptr(),
            style, x, y, width, height,
            null_mut(), null_mut(), INSTANCE, null(),
        );
        if !hwnd.is_null() && !overlay {
            ShowWindow(hwnd, SW_SHOW);
        }
        hwnd
    }

    unsafe fn start_capture(overlay: bool) {
        let source = selected_source_window();
        if source.is_null() || IsWindow(source) == 0 {
            set_status("Сначала выбери окно игры и обнови список, если его нет.");
            return;
        }

        if overlay && !HOTKEY_REGISTERED {
            set_status("Не удалось зарегистрировать Ctrl+Alt+F9. Оверлей не запущен ради безопасного выхода.");
            return;
        }

        stop_capture_only();
        if !PREVIEW_HWND.is_null() && IsWindow(PREVIEW_HWND) != 0 {
            DestroyWindow(PREVIEW_HWND);
        }
        CAPTURE_SOURCE_HWND = source;
        OVERLAY_MODE = overlay;
        PREVIEW_HWND = create_output_window(overlay, source);
        if PREVIEW_HWND.is_null() {
            OVERLAY_MODE = false;
            CAPTURE_SOURCE_HWND = null_mut();
            set_status("Не удалось создать окно вывода.");
            return;
        }

        let Some(shader_path) = current_shader_path() else {
            OVERLAY_MODE = false;
            CAPTURE_SOURCE_HWND = null_mut();
            set_status("Не найден файл шейдера FSR.");
            DestroyWindow(PREVIEW_HWND);
            return;
        };
        let mut error_text = [0u16; 512];
        CAPTURE_HANDLE = es_capture_start(
            source, PREVIEW_HWND, selected_profile() as u32,
            shader_path.as_ptr(), error_text.as_mut_ptr(), error_text.len() as i32,
        );

        if CAPTURE_HANDLE.is_null() {
            OVERLAY_MODE = false;
            CAPTURE_SOURCE_HWND = null_mut();
            set_status(&format!("Не удалось запустить захват: {}", from_wide(&error_text)));
            DestroyWindow(PREVIEW_HWND);
            return;
        }

        SetTimer(MAIN_HWND, 1, 1000, None);
        if overlay {
            SetTimer(MAIN_HWND, 2, 33, None);
            set_status("Оверлей включён. Ctrl+Alt+F9 — выключить. Тестируй в оконной игре.");
        } else {
            set_status("Захват запущен. Ниже будет FPS предпросмотра, а не самой игры.");
        }
    }

    unsafe fn stop_preview() {
        stop_capture_only();
        if !PREVIEW_HWND.is_null() && IsWindow(PREVIEW_HWND) != 0 {
            let hwnd = PREVIEW_HWND;
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
        }
        set_status("Захват остановлен.");
    }

    unsafe fn create_controls(parent: HWND) {
        create_label(parent, "END SCALING", 24, 18, 450, 32);
        GPU_LABEL = create_label(parent, "Проверка графического адаптера...", 24, 54, 500, 42);

        create_label(parent, "Профиль масштабирования", 24, 108, 260, 22);
        PROFILE_COMBO = CreateWindowExW(
            0, wide("COMBOBOX").as_ptr(), wide("").as_ptr(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            24, 132, 300, 150, parent, ID_PROFILE as _, INSTANCE, null(),
        );
        for value in ["Quality — качество", "Balanced — баланс", "Performance — минимум нагрузки"] {
            SendMessageW(PROFILE_COMBO, CB_ADDSTRING, 0, wide(value).as_ptr() as LPARAM);
        }
        SendMessageW(PROFILE_COMBO, CB_SETCURSEL, SELECTED_PROFILE as WPARAM, 0);
        RESOLUTION_LABEL = create_label(parent, "", 24, 165, 510, 30);
        create_button(parent, "Сохранить профиль", ID_SAVE, 24, 198, 180, 32, true);
        STATUS_LABEL = create_label(parent, "Пока не выбран источник изображения.", 24, 238, 510, 36);

        create_label(parent, "Окно игры или приложения для захвата", 24, 292, 340, 22);
        WINDOW_COMBO = CreateWindowExW(
            0, wide("COMBOBOX").as_ptr(), wide("").as_ptr(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            24, 317, 360, 180, parent, ID_WINDOW_LIST as _, INSTANCE, null(),
        );
        create_button(parent, "Обновить", ID_REFRESH, 396, 317, 120, 32, false);
        create_button(parent, "Начать предпросмотр", ID_START, 24, 365, 225, 36, true);
        create_button(parent, "Тестовый оверлей", ID_OVERLAY, 264, 365, 170, 36, false);
        create_button(parent, "Остановить", ID_STOP, 24, 410, 140, 34, false);

        probe_gpu();
        refresh_window_list();
    }

    unsafe extern "system" fn main_window_proc(
        hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM,
    ) -> LRESULT {
        match message {
            WM_CREATE => {
                create_controls(hwnd);
                0
            }
            WM_TIMER if wparam == 1 => {
                update_capture_stats();
                0
            }
            WM_TIMER if wparam == 2 => {
                update_overlay_position();
                0
            }
            WM_HOTKEY if wparam == HOTKEY_EMERGENCY_STOP as usize => {
                stop_preview();
                0
            }
            WM_COMMAND => {
                let id = (wparam & 0xffff) as usize;
                let notification = ((wparam >> 16) & 0xffff) as u16;
                match id {
                    ID_SAVE => { save_profile(); 0 }
                    ID_REFRESH => { refresh_window_list(); 0 }
                    ID_START => { start_capture(false); 0 }
                    ID_OVERLAY => { start_capture(true); 0 }
                    ID_STOP => { stop_preview(); 0 }
                    ID_PROFILE | ID_WINDOW_LIST if notification == CBN_SELCHANGE => {
                        update_resolution_label();
                        0
                    }
                    _ => DefWindowProcW(hwnd, message, wparam, lparam),
                }
            }
            WM_DESTROY => {
                stop_capture_only();
                if HOTKEY_REGISTERED {
                    UnregisterHotKey(hwnd, HOTKEY_EMERGENCY_STOP);
                    HOTKEY_REGISTERED = false;
                }
                if !PREVIEW_HWND.is_null() && IsWindow(PREVIEW_HWND) != 0 {
                    DestroyWindow(PREVIEW_HWND);
                }
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
            let class_name = wide("EndScalingMainWindow");
            let title = wide("End Scaling");

            let class = WNDCLASSW {
                style: CS_HREDRAW | CS_VREDRAW,
                lpfnWndProc: Some(main_window_proc),
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

            MAIN_HWND = CreateWindowExW(
                0, class_name.as_ptr(), title.as_ptr(),
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                    WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VISIBLE,
                CW_USEDEFAULT, CW_USEDEFAULT, 570, 500,
                null_mut(), null_mut(), INSTANCE, null(),
            );
            if MAIN_HWND.is_null() {
                return;
            }
            ShowWindow(MAIN_HWND, SW_SHOW);
            HOTKEY_REGISTERED = RegisterHotKey(
                MAIN_HWND, HOTKEY_EMERGENCY_STOP, MOD_CONTROL_ALT, VK_F9,
            ) != 0;

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
