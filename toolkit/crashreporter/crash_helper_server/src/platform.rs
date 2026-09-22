/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#[cfg(target_os = "android")]
mod android;

#[cfg(target_os = "android")]
pub(crate) use android::PROXY_RENDEZ_VOUS;

#[cfg(any(target_os = "macos"))]
mod macos;

#[cfg(any(target_os = "macos"))]
pub(crate) use macos::CRASH_REPORTER_PATH;

#[cfg(any(target_os = "linux", target_os = "macos"))]
mod unix;

#[cfg(any(target_os = "linux", target_os = "macos"))]
pub(crate) use unix::{
    daemonize, get_client_handle, CRASH_REPORTER_FILENAME as CRASH_REPORTER_PATH, PROXY_RENDEZ_VOUS,
};

#[cfg(target_os = "windows")]
mod windows;

#[cfg(target_os = "windows")]
pub(crate) use windows::{
    daemonize, get_client_handle, CRASH_REPORTER_FILENAME, PROXY_RENDEZ_VOUS,
};
