#![cfg_attr(windows, windows_subsystem = "windows")]

#[cfg(windows)]
mod app {
    use std::{
        env, ffi::{c_void, OsStr}, fs, os::windows::ffi::OsStrExt,
        path::PathBuf, ptr::{null, null_mut},
    };

    use windows_sys::Win32::{
        UI::Input::KeyboardAndMouse::{RegisterHotKey, UnregisterHotKey},
        Foundation::{BOOL, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM},
        Graphics::Gdi::{ClientToScreen, GetStockObject, HBRUSH, BLACK_BRUSH, WHITE_BRUSH},
        System::{LibraryLoader::GetModuleHandleW, Threading::GetCurrentProcessId},
        UI::WindowsAndMessaging::{
            CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, EnumWindows,
            GetClientRect, GetMessageW, GetSystemMetrics, GetWindowTextLengthW, GetWindowTextW,
            GetWindowThreadProcessId, GetWindowRect, GetForegroundWindow, IsIconic, IsZoomed, IsWindow,
            IsWindowVisible, KillTimer, PostMessageW, PostQuitMessage, RegisterClassW, SendMessageW,
            SetTimer, SetWindowPos, SetWindowTextW, ShowWindow,
            TranslateMessage, CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, HWND_TOPMOST, MSG,
            SW_HIDE, SW_SHOW, SWP_NOACTIVATE, SWP_NOZORDER, SWP_SHOWWINDOW, WNDCLASSW,
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
    const ID_APPLY_RESOLUTION: usize = 1008;
    const ID_RESTORE_RESOLUTION: usize = 1009;
    const HOTKEY_EMERGENCY_STOP: i32 = 1;
    const MOD_NOREPEAT: u32 = 0x4000;
    const MOD_CONTROL_ALT: u32 = 0x0003 | MOD_NOREPEAT;
    const VK_F9: u32 = 0x78;
    const WS_POPUP: u32 = 0x80000000;

    const WM_MOUSEACTIVATE_VALUE: u32 = 0x0021;
    const WM_MOUSEMOVE_VALUE: u32 = 0x0200;
    const WM_LBUTTONDOWN_VALUE: u32 = 0x0201;
    const WM_LBUTTONUP_VALUE: u32 = 0x0202;
    const WM_LBUTTONDBLCLK_VALUE: u32 = 0x0203;
    const WM_RBUTTONDOWN_VALUE: u32 = 0x0204;
    const WM_RBUTTONUP_VALUE: u32 = 0x0205;
    const WM_RBUTTONDBLCLK_VALUE: u32 = 0x0206;
    const WM_MBUTTONDOWN_VALUE: u32 = 0x0207;
    const WM_MBUTTONUP_VALUE: u32 = 0x0208;
    const WM_MBUTTONDBLCLK_VALUE: u32 = 0x0209;
    const WM_MOUSEWHEEL_VALUE: u32 = 0x020A;
    const WM_XBUTTONDOWN_VALUE: u32 = 0x020B;
    const WM_XBUTTONUP_VALUE: u32 = 0x020C;
    const WM_XBUTTONDBLCLK_VALUE: u32 = 0x020D;
    const WM_MOUSEHWHEEL_VALUE: u32 = 0x020E;

    const MA_NOACTIVATE_VALUE: LRESULT = 3;

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
    static mut RESOLUTION_APPLIED: bool = false;
    static mut RESIZED_HWND: HWND = null_mut();
    static mut ORIGINAL_WINDOW_RECT: RECT = RECT { left: 0, top: 0, right: 0, bottom: 0 };
    static mut ORIGINAL_CLIENT_WIDTH: i32 = 0;
    static mut ORIGINAL_CLIENT_HEIGHT: i32 = 0;
    static mut ORIGINAL_PROCESS_ID: u32 = 0;

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
        fn es_capture_set_mode(handle: *mut c_void, mode: u32);
        fn es_capture_get_stats(
            handle: *mut c_void,
            fps: *mut f64,
            submit_ms: *mut f64,
            gpu_ms: *mut f64,
            frames: *mut u64,
            dropped: *mut u64,
            inactive: *mut u64,
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

    fn scale_pointer_coordinate(position: i32, output_extent: i32, source_extent: i32) -> i32 {
        if output_extent <= 0 || source_extent <= 0 {
            return 0;
        }
        ((position as i64 * source_extent as i64) / output_extent as i64)
            .clamp(i32::MIN as i64, i32::MAX as i64) as i32
    }

    fn pack_mouse_coordinates(x: i32, y: i32) -> LPARAM {
        let x = x.clamp(i16::MIN as i32, i16::MAX as i32) as i16 as u16 as u32;
        let y = y.clamp(i16::MIN as i32, i16::MAX as i32) as i16 as u16 as u32;
        ((y << 16) | x) as LPARAM
    }

    fn signed_low_word(value: LPARAM) -> i32 {
        (value as u32 as u16 as i16) as i32
    }

    fn signed_high_word(value: LPARAM) -> i32 {
        ((value as u32 >> 16) as u16 as i16) as i32
    }

    fn scaled_client_size(width: i32, height: i32, scale: f32) -> Option<(i32, i32)> {
        if width < 320 || height < 240 || !scale.is_finite() || !(0.0..1.0).contains(&scale) {
            return None;
        }
        let scaled_width = (((width as f32 * scale) as i32) / 8 * 8).max(320);
        let scaled_height = (((height as f32 * scale) as i32) / 8 * 8).max(240);
        if scaled_width >= width || scaled_height >= height {
            return None;
        }
        Some((scaled_width, scaled_height))
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
        let (width, height, current_width, current_height) =
            if !source.is_null() && IsWindow(source) != 0 &&
                GetClientRect(source, &mut rect) != 0 &&
                rect.right > rect.left && rect.bottom > rect.top {
                let current_width = rect.right - rect.left;
                let current_height = rect.bottom - rect.top;
                if RESOLUTION_APPLIED && source == RESIZED_HWND {
                    (ORIGINAL_CLIENT_WIDTH, ORIGINAL_CLIENT_HEIGHT, current_width, current_height)
                } else {
                    (current_width, current_height, current_width, current_height)
                }
            } else {
                let width = GetSystemMetrics(SM_CXSCREEN).max(1);
                let height = GetSystemMetrics(SM_CYSCREEN).max(1);
                (width, height, width, height)
            };

        let text = match scaled_client_size(width, height, profile_scale(selected_profile())) {
            Some((target_width, target_height)) if RESOLUTION_APPLIED && source == RESIZED_HWND =>
                format!("Цель: {target_width} × {target_height}  •  сейчас: {current_width} × {current_height}"),
            Some((target_width, target_height)) =>
                format!("Размер окна игры: {target_width} × {target_height}  (сейчас: {current_width} × {current_height})"),
            None => format!("Окно слишком маленькое для уменьшения ({current_width} × {current_height})"),
        };
        SetWindowTextW(RESOLUTION_LABEL, wide(&text).as_ptr());
    }

    fn settings_path() -> Option<PathBuf> {
        Some(PathBuf::from(env::var_os("APPDATA")?)
            .join("EndScaling")
            .join("settings.ini"))
    }

    fn resolution_recovery_path() -> Option<PathBuf> {
        Some(PathBuf::from(env::var_os("APPDATA")?)
            .join("EndScaling")
            .join("window_restore.ini"))
    }

    fn clear_resolution_recovery() {
        if let Some(path) = resolution_recovery_path() {
            let _ = fs::remove_file(path);
        }
    }

    fn write_resolution_recovery(hwnd: HWND, pid: u32, rect: RECT) -> bool {
        let Some(path) = resolution_recovery_path() else { return false; };
        let Some(parent) = path.parent() else { return false; };
        if fs::create_dir_all(parent).is_err() { return false; }
        let contents = format!(
            "pid={pid}\nhwnd={}\nleft={}\ntop={}\nright={}\nbottom={}\n",
            hwnd as usize, rect.left, rect.top, rect.right, rect.bottom
        );
        fs::write(path, contents).is_ok()
    }

    fn recovery_value(contents: &str, key: &str) -> Option<i64> {
        contents.lines()
            .find_map(|line| line.strip_prefix(&format!("{key}="))?.parse::<i64>().ok())
    }

    unsafe fn restore_pending_resolution() -> bool {
        let Some(path) = resolution_recovery_path() else { return false; };
        let Ok(contents) = fs::read_to_string(&path) else { return false; };
        let values = (
            recovery_value(&contents, "pid"),
            recovery_value(&contents, "hwnd"),
            recovery_value(&contents, "left"),
            recovery_value(&contents, "top"),
            recovery_value(&contents, "right"),
            recovery_value(&contents, "bottom"),
        );
        let (Some(pid), Some(hwnd_value), Some(left), Some(top), Some(right), Some(bottom)) = values else {
            let _ = fs::remove_file(path);
            return false;
        };
        if pid <= 0 || hwnd_value <= 0 || right <= left || bottom <= top ||
            left < i32::MIN as i64 || top < i32::MIN as i64 ||
            right > i32::MAX as i64 || bottom > i32::MAX as i64 ||
            right - left > 32768 || bottom - top > 32768 {
            let _ = fs::remove_file(path);
            return false;
        }

        let hwnd = hwnd_value as usize as HWND;
        if IsWindow(hwnd) == 0 {
            let _ = fs::remove_file(path);
            return false;
        }
        let mut actual_pid = 0u32;
        GetWindowThreadProcessId(hwnd, &mut actual_pid);
        if actual_pid as i64 != pid {
            let _ = fs::remove_file(path);
            return false;
        }

        let mut current = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        if GetWindowRect(hwnd, &mut current) == 0 { return false; }
        let original_width = (right - left) as i32;
        let original_height = (bottom - top) as i32;
        let current_width = current.right - current.left;
        let current_height = current.bottom - current.top;
        // Avoid restoring a reused HWND or a window the user has since resized/moved.
        if current.left != left as i32 || current.top != top as i32 ||
            current_width <= 0 || current_height <= 0 ||
            current_width >= original_width || current_height >= original_height {
            let _ = fs::remove_file(path);
            return false;
        }

        let restored = SetWindowPos(
            hwnd, null_mut(), left as i32, top as i32,
            original_width, original_height, SWP_NOZORDER | SWP_NOACTIVATE,
        ) != 0;
        if restored { let _ = fs::remove_file(path); }
        restored
    }

    unsafe fn restore_source_resolution() -> bool {
        if !RESOLUTION_APPLIED || RESIZED_HWND.is_null() { return false; }
        let hwnd = RESIZED_HWND;
        if IsWindow(hwnd) == 0 {
            clear_resolution_recovery();
        } else {
            let mut actual_pid = 0u32;
            GetWindowThreadProcessId(hwnd, &mut actual_pid);
            if actual_pid != ORIGINAL_PROCESS_ID {
                clear_resolution_recovery();
            } else {
                let rect = ORIGINAL_WINDOW_RECT;
                let restored = SetWindowPos(
                    hwnd, null_mut(), rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top,
                    SWP_NOZORDER | SWP_NOACTIVATE,
                ) != 0;
                if !restored { return false; }
                clear_resolution_recovery();
            }
        }

        RESOLUTION_APPLIED = false;
        RESIZED_HWND = null_mut();
        ORIGINAL_WINDOW_RECT = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        ORIGINAL_CLIENT_WIDTH = 0;
        ORIGINAL_CLIENT_HEIGHT = 0;
        ORIGINAL_PROCESS_ID = 0;
        update_resolution_label();
        true
    }

    unsafe fn apply_source_resolution() {
        let source = selected_source_window();
        if source.is_null() || IsWindow(source) == 0 {
            set_status("Сначала выбери окно игры или приложения.");
            return;
        }
        if RESOLUTION_APPLIED && RESIZED_HWND != source && !restore_source_resolution() {
            set_status("Не удалось восстановить предыдущее окно. Сначала нажми «Вернуть размер».");
            return;
        }
        if IsIconic(source) != 0 || IsZoomed(source) != 0 {
            set_status("Для изменения размера сначала верни игру в обычный оконный режим.");
            return;
        }

        let mut current_rect = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        let mut client_rect = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        if GetWindowRect(source, &mut current_rect) == 0 ||
            GetClientRect(source, &mut client_rect) == 0 {
            set_status("Не удалось прочитать размеры выбранного окна.");
            return;
        }

        let (original_rect, base_width, base_height, pid) =
            if RESOLUTION_APPLIED && RESIZED_HWND == source {
                (ORIGINAL_WINDOW_RECT, ORIGINAL_CLIENT_WIDTH, ORIGINAL_CLIENT_HEIGHT, ORIGINAL_PROCESS_ID)
            } else {
                let mut process_id = 0u32;
                GetWindowThreadProcessId(source, &mut process_id);
                (current_rect, client_rect.right - client_rect.left,
                 client_rect.bottom - client_rect.top, process_id)
            };
        let Some((target_width, target_height)) =
            scaled_client_size(base_width, base_height, profile_scale(selected_profile())) else {
            set_status("Окно слишком маленькое или его нельзя уменьшить с этим профилем.");
            return;
        };

        let client_width = client_rect.right - client_rect.left;
        let client_height = client_rect.bottom - client_rect.top;
        let frame_width = current_rect.right - current_rect.left - client_width;
        let frame_height = current_rect.bottom - current_rect.top - client_height;
        if frame_width < 0 || frame_height < 0 || pid == 0 {
            set_status("Не удалось определить рамку или процесс выбранного окна.");
            return;
        }
        if !RESOLUTION_APPLIED && !write_resolution_recovery(source, pid, original_rect) {
            set_status("Не удалось сохранить точку восстановления исходного размера.");
            return;
        }

        let changed = SetWindowPos(
            source, null_mut(), current_rect.left, current_rect.top,
            target_width + frame_width, target_height + frame_height,
            SWP_NOZORDER | SWP_NOACTIVATE,
        ) != 0;
        if !changed {
            if !RESOLUTION_APPLIED { clear_resolution_recovery(); }
            set_status("Windows не разрешила изменить размер окна игры.");
            return;
        }

        let mut verify = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        if GetClientRect(source, &mut verify) == 0 ||
            verify.right - verify.left >= base_width ||
            verify.bottom - verify.top >= base_height {
            let _ = SetWindowPos(
                source, null_mut(), original_rect.left, original_rect.top,
                original_rect.right - original_rect.left,
                original_rect.bottom - original_rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE,
            );
            clear_resolution_recovery();
            RESOLUTION_APPLIED = false;
            RESIZED_HWND = null_mut();
            set_status("Игра не приняла уменьшенный размер окна. Исходный размер возвращён.");
            update_resolution_label();
            return;
        }

        RESOLUTION_APPLIED = true;
        RESIZED_HWND = source;
        ORIGINAL_WINDOW_RECT = original_rect;
        ORIGINAL_CLIENT_WIDTH = base_width;
        ORIGINAL_CLIENT_HEIGHT = base_height;
        ORIGINAL_PROCESS_ID = pid;
        update_resolution_label();
        set_status(&format!(
            "Окно уменьшено до {} × {}. Остановка предпросмотра или закрытие End-scaling восстановит исходный размер.",
            verify.right - verify.left, verify.bottom - verify.top
        ));
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
                set_status("Профиль сохранён. Низкое разрешение можно применить к выбранному оконному приложению.");
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
        let mut dropped = 0u64;
        let mut inactive = 0u64;
        if es_capture_get_stats(
            CAPTURE_HANDLE,
            &mut preview_fps,
            &mut submit_ms,
            &mut gpu_ms,
            &mut frames,
            &mut dropped,
            &mut inactive,
        ) != 0 {
            let label = if OVERLAY_MODE {
                format!("Оверлей: {preview_fps:.1} FPS • GPU {gpu_ms:.2} ms • CPU {submit_ms:.2} ms\nКадры: {frames} • пропуски: {dropped} • скрыто: {inactive}")
            } else {
                format!("Вывод: {preview_fps:.1} FPS • GPU {gpu_ms:.2} ms • CPU {submit_ms:.2} ms\nКадры: {frames} • пропуски: {dropped}")
            };
            set_status(&label);
        }
    }

    unsafe extern "system" fn preview_window_proc(
        hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM,
    ) -> LRESULT {
        match message {
            WM_MOUSEACTIVATE_VALUE if OVERLAY_MODE => MA_NOACTIVATE_VALUE,
            WM_MOUSEMOVE_VALUE | WM_LBUTTONDOWN_VALUE | WM_LBUTTONUP_VALUE |
            WM_LBUTTONDBLCLK_VALUE | WM_RBUTTONDOWN_VALUE | WM_RBUTTONUP_VALUE |
            WM_RBUTTONDBLCLK_VALUE | WM_MBUTTONDOWN_VALUE | WM_MBUTTONUP_VALUE |
            WM_MBUTTONDBLCLK_VALUE | WM_MOUSEWHEEL_VALUE | WM_MOUSEHWHEEL_VALUE |
            WM_XBUTTONDOWN_VALUE | WM_XBUTTONUP_VALUE | WM_XBUTTONDBLCLK_VALUE
                if OVERLAY_MODE && !CAPTURE_SOURCE_HWND.is_null() => {
                    forward_overlay_mouse_message(
                        hwnd, CAPTURE_SOURCE_HWND, message, wparam, lparam,
                    );
                    0
                }
            WM_CLOSE => {
                stop_capture_only();
                restore_source_resolution();
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

    unsafe fn overlay_target_rect(source: HWND) -> Option<RECT> {
        let mut client = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        if GetClientRect(source, &mut client) == 0 {
            return None;
        }

        let (width, height) = if RESOLUTION_APPLIED && source == RESIZED_HWND {
            (ORIGINAL_CLIENT_WIDTH, ORIGINAL_CLIENT_HEIGHT)
        } else {
            (client.right - client.left, client.bottom - client.top)
        };
        if width <= 0 || height <= 0 {
            return None;
        }

        let mut origin = POINT { x: 0, y: 0 };
        if ClientToScreen(source, &mut origin) == 0 {
            return None;
        }
        Some(RECT {
            left: origin.x,
            top: origin.y,
            right: origin.x.saturating_add(width),
            bottom: origin.y.saturating_add(height),
        })
    }

    unsafe fn update_overlay_position() {
        if !OVERLAY_MODE || PREVIEW_HWND.is_null() || CAPTURE_SOURCE_HWND.is_null() ||
            IsWindow(PREVIEW_HWND) == 0 || IsWindow(CAPTURE_SOURCE_HWND) == 0 {
            return;
        }

        // Align to the original client area, not the deliberately reduced source.
        let should_show = IsWindowVisible(CAPTURE_SOURCE_HWND) != 0 &&
            IsIconic(CAPTURE_SOURCE_HWND) == 0 &&
            GetForegroundWindow() == CAPTURE_SOURCE_HWND;
        if !should_show {
            if IsWindowVisible(PREVIEW_HWND) != 0 {
                ShowWindow(PREVIEW_HWND, SW_HIDE);
            }
            return;
        }

        let Some(rect) = overlay_target_rect(CAPTURE_SOURCE_HWND) else {
            ShowWindow(PREVIEW_HWND, SW_HIDE);
            return;
        };
        let width = rect.right - rect.left;
        let height = rect.bottom - rect.top;

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

    unsafe fn forward_overlay_mouse_message(
        overlay: HWND, source: HWND, message: u32, wparam: WPARAM, lparam: LPARAM,
    ) {
        if IsWindow(source) == 0 || IsWindow(overlay) == 0 {
            return;
        }

        let mut output_client = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        let mut source_client = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        if GetClientRect(overlay, &mut output_client) == 0 ||
            GetClientRect(source, &mut source_client) == 0 {
            return;
        }

        let output_width = output_client.right - output_client.left;
        let output_height = output_client.bottom - output_client.top;
        let source_width = source_client.right - source_client.left;
        let source_height = source_client.bottom - source_client.top;
        if output_width <= 0 || output_height <= 0 || source_width <= 0 || source_height <= 0 {
            return;
        }

        let is_wheel = message == WM_MOUSEWHEEL_VALUE || message == WM_MOUSEHWHEEL_VALUE;
        let (local_x, local_y) = if is_wheel {
            let mut output_origin = POINT { x: 0, y: 0 };
            if ClientToScreen(overlay, &mut output_origin) == 0 {
                return;
            }
            (
                signed_low_word(lparam).saturating_sub(output_origin.x),
                signed_high_word(lparam).saturating_sub(output_origin.y),
            )
        } else {
            (signed_low_word(lparam), signed_high_word(lparam))
        };

        let source_x = scale_pointer_coordinate(local_x, output_width, source_width);
        let source_y = scale_pointer_coordinate(local_y, output_height, source_height);
        let forwarded_lparam = if is_wheel {
            let mut source_point = POINT { x: source_x, y: source_y };
            if ClientToScreen(source, &mut source_point) == 0 {
                return;
            }
            pack_mouse_coordinates(source_point.x, source_point.y)
        } else {
            pack_mouse_coordinates(source_x, source_y)
        };

        // Only basic Win32 mouse messages are synthesized. Raw Input and
        // global drag capture are intentionally not intercepted by this path.
        PostMessageW(source, message, wparam, forwarded_lparam);
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
            let Some(rect) = overlay_target_rect(source) else {
                return null_mut();
            };
            x = rect.left;
            y = rect.top;
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }

        // Keep the flip-model HWND path. The overlay is topmost and NOACTIVATE,
        // but receives mouse input so it can translate coordinates to the source.
        // Keyboard focus remains with the game window underneath.
        let extended_style = if overlay {
            0x00000008 | 0x00000080 | 0x08000000
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
        let restored = restore_source_resolution();
        set_status(if restored {
            "Захват остановлен. Исходный размер окна игры восстановлен."
        } else {
            "Захват остановлен."
        });
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
        create_button(parent, "Применить низкое разрешение", ID_APPLY_RESOLUTION, 24, 455, 250, 34, false);
        create_button(parent, "Вернуть размер", ID_RESTORE_RESOLUTION, 290, 455, 160, 34, false);

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
                    ID_APPLY_RESOLUTION => { apply_source_resolution(); 0 }
                    ID_RESTORE_RESOLUTION => {
                        if restore_source_resolution() {
                            set_status("Исходный размер окна восстановлен.");
                        } else {
                            set_status("Нет изменённого окна или Windows не смогла его восстановить.");
                        }
                        0
                    }
                    ID_PROFILE if notification == CBN_SELCHANGE => {
                        update_resolution_label();
                        if RESOLUTION_APPLIED { apply_source_resolution(); }
                        if !CAPTURE_HANDLE.is_null() {
                            es_capture_set_mode(CAPTURE_HANDLE, selected_profile() as u32);
                            set_status("Профиль масштабирования переключён.");
                        }
                        0
                    }
                    ID_WINDOW_LIST if notification == CBN_SELCHANGE => {
                        if RESOLUTION_APPLIED && selected_source_window() != RESIZED_HWND {
                            restore_source_resolution();
                        }
                        update_resolution_label();
                        0
                    }
                    _ => DefWindowProcW(hwnd, message, wparam, lparam),
                }
            }
            WM_DESTROY => {
                stop_capture_only();
                restore_source_resolution();
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
                CW_USEDEFAULT, CW_USEDEFAULT, 570, 535,
                null_mut(), null_mut(), INSTANCE, null(),
            );
            if MAIN_HWND.is_null() {
                return;
            }
            ShowWindow(MAIN_HWND, SW_SHOW);
            if restore_pending_resolution() {
                set_status("После предыдущего завершения найдено уменьшенное окно — исходный размер восстановлен.");
            }
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

    #[cfg(test)]
    mod tests {
        use super::scaled_client_size;

        #[test]
        fn balanced_profile_scales_and_aligns_dimensions() {
            assert_eq!(scaled_client_size(1920, 1080, 0.67), Some((1280, 720)));
        }

        #[test]
        fn performance_profile_respects_minimum_dimensions() {
            assert_eq!(scaled_client_size(1280, 720, 0.50), Some((640, 360)));
            assert_eq!(scaled_client_size(640, 480, 0.50), Some((320, 240)));
        }

        #[test]
        fn too_small_or_invalid_windows_are_rejected() {
            assert_eq!(scaled_client_size(300, 600, 0.50), None);
            assert_eq!(scaled_client_size(1280, 720, 1.0), None);
            assert_eq!(scaled_client_size(1280, 720, f32::NAN), None);
        }

        #[test]
        fn pointer_coordinates_scale_from_output_to_source() {
            assert_eq!(scale_pointer_coordinate(960, 1920, 1280), 640);
            assert_eq!(scale_pointer_coordinate(540, 1080, 720), 360);
            assert_eq!(scale_pointer_coordinate(1920, 1920, 1280), 1280);
        }

        #[test]
        fn pointer_coordinates_preserve_drag_positions_outside_output() {
            assert_eq!(scale_pointer_coordinate(-15, 1920, 1280), -10);
            assert_eq!(scale_pointer_coordinate(0, 0, 1280), 0);
            assert_eq!(scale_pointer_coordinate(10, 1920, 0), 0);
        }

        #[test]
        fn packed_mouse_coordinates_keep_signed_values() {
            assert_eq!(signed_low_word(pack_mouse_coordinates(-12, 37)), -12);
            assert_eq!(signed_high_word(pack_mouse_coordinates(-12, 37)), 37);
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
