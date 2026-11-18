use std::ffi::c_void;
use std::fs;

use serde::{Deserialize, Serialize};
use url::Url;

const MAX_FEED_BYTES: usize = 1024 * 1024;
const USER_AGENT: &str = concat!("ZEND-Updater/", env!("CARGO_PKG_VERSION"));

const WINHTTP_QUERY_STATUS_CODE: u32 = 19;
const WINHTTP_QUERY_FLAG_NUMBER: u32 = 0x2000_0000;
const WINHTTP_FLAG_SECURE: u32 = 0x0080_0000;

#[derive(Serialize)]
pub struct UpdateCheck {
    pub current: String,
    pub latest: String,
    pub available: bool,
    pub url: String,
    pub notes: String,
    pub published: String,
    pub source: String,
}

#[derive(Deserialize)]
struct Feed {
    version: String,
    #[serde(default)]
    url: String,
    #[serde(default)]
    notes: String,
    #[serde(default)]
    published: String,
}

struct Handle(*mut c_void);

impl Drop for Handle {
    fn drop(&mut self) {
        unsafe {
            WinHttpCloseHandle(self.0);
        }
    }
}

#[link(name = "winhttp")]
unsafe extern "system" {
    fn WinHttpOpen(
        agent: *const u16,
        access_type: u32,
        proxy: *const u16,
        bypass: *const u16,
        flags: u32,
    ) -> *mut c_void;
    fn WinHttpConnect(
        session: *mut c_void,
        host: *const u16,
        port: u16,
        reserved: u32,
    ) -> *mut c_void;
    fn WinHttpOpenRequest(
        connect: *mut c_void,
        verb: *const u16,
        target: *const u16,
        version: *const u16,
        referrer: *const u16,
        accept_types: *const *const u16,
        flags: u32,
    ) -> *mut c_void;
    fn WinHttpSendRequest(
        request: *mut c_void,
        headers: *const u16,
        headers_len: u32,
        optional: *const c_void,
        optional_len: u32,
        total_len: u32,
        context: usize,
    ) -> i32;
    fn WinHttpReceiveResponse(request: *mut c_void, reserved: *mut c_void) -> i32;
    fn WinHttpQueryHeaders(
        request: *mut c_void,
        info_level: u32,
        name: *const u16,
        buffer: *mut c_void,
        buffer_len: *mut u32,
        index: *mut u32,
    ) -> i32;
    fn WinHttpReadData(
        request: *mut c_void,
        buffer: *mut c_void,
        to_read: u32,
        read: *mut u32,
    ) -> i32;
    fn WinHttpSetTimeouts(
        handle: *mut c_void,
        resolve: i32,
        connect: i32,
        send: i32,
        receive: i32,
    ) -> i32;
    fn WinHttpCloseHandle(handle: *mut c_void) -> i32;
}

#[link(name = "shell32")]
unsafe extern "system" {
    fn ShellExecuteW(
        window: *mut c_void,
        operation: *const u16,
        file: *const u16,
        parameters: *const u16,
        directory: *const u16,
        show: i32,
    ) -> isize;
}

#[tauri::command]
pub async fn update_check(feed: String) -> Result<UpdateCheck, String> {
    tauri::async_runtime::spawn_blocking(move || check(&feed))
        .await
        .map_err(|error| format!("update task failed: {error}"))?
}

#[tauri::command]
pub fn update_open(url: String) -> Result<(), String> {
    let lower = url.to_ascii_lowercase();
    if !(lower.starts_with("https://") || lower.starts_with("http://")) {
        return Err("only http(s) download links are allowed".to_string());
    }
    let operation = wide("open");
    let target = wide(&url);
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            operation.as_ptr(),
            target.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
        )
    };
    if result > 32 {
        Ok(())
    } else {
        Err(format!("could not open the download link ({result})"))
    }
}

fn check(feed: &str) -> Result<UpdateCheck, String> {
    let feed = feed.trim();
    if feed.is_empty() {
        return Err("update feed is empty".to_string());
    }

    let (text, source) = if feed.starts_with("http://") || feed.starts_with("https://") {
        (http_get(feed)?, "remote".to_string())
    } else {
        (read_local(feed)?, "file".to_string())
    };

    let parsed: Feed =
        serde_json::from_str(&text).map_err(|error| format!("feed is not valid JSON: {error}"))?;
    let latest = parse_version(&parsed.version)
        .ok_or_else(|| format!("feed version {:?} is not a version", parsed.version))?;
    let current = parse_version(env!("CARGO_PKG_VERSION")).expect("package version is valid");

    Ok(UpdateCheck {
        current: env!("CARGO_PKG_VERSION").to_string(),
        latest: parsed.version.trim().to_string(),
        available: latest > current,
        url: parsed.url.trim().to_string(),
        notes: parsed.notes.trim().to_string(),
        published: parsed.published.trim().to_string(),
        source,
    })
}

fn read_local(feed: &str) -> Result<String, String> {
    let path = feed.strip_prefix("file://").unwrap_or(feed);
    let trimmed = path.strip_prefix('/').unwrap_or(path);
    let path = if trimmed.as_bytes().get(1) == Some(&b':') {
        trimmed
    } else {
        path
    };

    let meta = fs::metadata(path).map_err(|error| format!("cannot read {path}: {error}"))?;
    if meta.len() > MAX_FEED_BYTES as u64 {
        return Err(format!("feed is larger than {MAX_FEED_BYTES} bytes"));
    }
    fs::read_to_string(path).map_err(|error| format!("cannot read {path}: {error}"))
}

fn http_get(feed: &str) -> Result<String, String> {
    let parsed = Url::parse(feed).map_err(|error| format!("bad feed URL: {error}"))?;
    let scheme = parsed.scheme();
    if scheme != "http" && scheme != "https" {
        return Err(format!("unsupported feed scheme {scheme:?}"));
    }
    let host = parsed.host_str().ok_or("feed URL has no host")?;
    let port = parsed
        .port_or_known_default()
        .ok_or("feed URL has no port")?;

    let mut target = parsed.path().to_string();
    if target.is_empty() {
        target.push('/');
    }
    if let Some(query) = parsed.query() {
        target.push('?');
        target.push_str(query);
    }

    let agent = wide(USER_AGENT);
    let host = wide(host);
    let target = wide(&target);
    let verb = wide("GET");
    let flags = if scheme == "https" {
        WINHTTP_FLAG_SECURE
    } else {
        0
    };

    unsafe {
        let session = WinHttpOpen(agent.as_ptr(), 0, std::ptr::null(), std::ptr::null(), 0);
        if session.is_null() {
            return Err(last_error("WinHttpOpen"));
        }
        let session = Handle(session);
        let _ = WinHttpSetTimeouts(session.0, 5_000, 5_000, 10_000, 10_000);

        let connect = WinHttpConnect(session.0, host.as_ptr(), port, 0);
        if connect.is_null() {
            return Err(last_error("WinHttpConnect"));
        }
        let connect = Handle(connect);

        let request = WinHttpOpenRequest(
            connect.0,
            verb.as_ptr(),
            target.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
            flags,
        );
        if request.is_null() {
            return Err(last_error("WinHttpOpenRequest"));
        }
        let request = Handle(request);

        if WinHttpSendRequest(
            request.0,
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            0,
            0,
        ) == 0
        {
            return Err(last_error("WinHttpSendRequest"));
        }
        if WinHttpReceiveResponse(request.0, std::ptr::null_mut()) == 0 {
            return Err(last_error("WinHttpReceiveResponse"));
        }

        let mut status: u32 = 0;
        let mut length: u32 = std::mem::size_of::<u32>() as u32;
        if WinHttpQueryHeaders(
            request.0,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            std::ptr::null(),
            &mut status as *mut u32 as *mut c_void,
            &mut length,
            std::ptr::null_mut(),
        ) == 0
        {
            return Err(last_error("WinHttpQueryHeaders"));
        }
        if status != 200 {
            return Err(format!("feed returned HTTP {status}"));
        }

        let mut body = Vec::new();
        let mut buffer = [0u8; 8192];
        loop {
            let mut read: u32 = 0;
            if WinHttpReadData(
                request.0,
                buffer.as_mut_ptr() as *mut c_void,
                buffer.len() as u32,
                &mut read,
            ) == 0
            {
                return Err(last_error("WinHttpReadData"));
            }
            if read == 0 {
                break;
            }
            body.extend_from_slice(&buffer[..read as usize]);
            if body.len() > MAX_FEED_BYTES {
                return Err(format!("feed is larger than {MAX_FEED_BYTES} bytes"));
            }
        }

        String::from_utf8(body).map_err(|error| format!("feed is not UTF-8: {error}"))
    }
}

fn parse_version(raw: &str) -> Option<(u64, u64, u64)> {
    let text = raw
        .trim()
        .trim_start_matches(|c| c == 'v' || c == 'V');
    let core = text.split(['-', '+']).next()?;
    let mut parts = core.split('.');
    let major = parts.next()?.parse().ok()?;
    let minor = parts.next().unwrap_or("0").parse().ok()?;
    let patch = parts.next().unwrap_or("0").parse().ok()?;
    Some((major, minor, patch))
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

fn last_error(api: &str) -> String {
    format!("{api} failed: {}", std::io::Error::last_os_error())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_compare() {
        assert!(parse_version("v0.2.0") > parse_version("0.1.9"));
        assert_eq!(parse_version("1.2"), Some((1, 2, 0)));
        assert_eq!(parse_version("1.2.3-beta.4"), Some((1, 2, 3)));
        assert!(parse_version("nightly").is_none());
    }

    #[test]
    fn local_feed_is_read() {
        let path = std::env::temp_dir().join(format!("zend-update-{}.json", std::process::id()));
        fs::write(
            &path,
            r#"{"version":"99.0.0","url":"https://example.com/ZEND-setup.exe","notes":"test","published":"2026-11-18"}"#,
        )
        .expect("write feed");
        let result = check(path.to_str().expect("utf-8 path")).expect("check feed");
        let _ = fs::remove_file(&path);
        assert!(result.available);
        assert_eq!(result.source, "file");
        assert_eq!(result.latest, "99.0.0");
    }

    #[test]
    #[ignore = "network access"]
    fn remote_feed_is_read() {
        let text = http_get("https://raw.githubusercontent.com/0xLLVEL/zend/main/updates.json")
            .expect("fetch feed");
        assert!(text.contains("\"version\""), "unexpected body: {text}");
    }
}
