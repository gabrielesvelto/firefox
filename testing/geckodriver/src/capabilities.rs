/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::command::LogOptions;
use crate::logging::Level;
use crate::marionette::MarionetteSettings;
use base64::prelude::BASE64_STANDARD;
use base64::Engine;
use mozdevice::AndroidStorageInput;
use mozprofile::preferences::Pref;
use mozprofile::profile::Profile;
use mozrunner::firefox_args::{get_arg_value, parse_args, Arg};
use mozrunner::runner::platform::firefox_default_path;
use mozversion::{firefox_binary_version, firefox_version, Version};
use regex::bytes::Regex;
use serde_json::{Map, Value};
use std::collections::BTreeMap;
use std::default::Default;
use std::ffi::OsString;
use std::fs;
use std::io;
use std::io::BufWriter;
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::str::{self, FromStr};
use thiserror::Error;
use webdriver::capabilities::{BrowserCapabilities, Capabilities};
use webdriver::error::{ErrorStatus, WebDriverError, WebDriverResult};

#[derive(Clone, Debug, Error)]
enum VersionError {
    #[error(transparent)]
    VersionError(#[from] mozversion::Error),
    #[error("No binary provided")]
    MissingBinary,
}

impl From<VersionError> for WebDriverError {
    fn from(err: VersionError) -> WebDriverError {
        WebDriverError::new(ErrorStatus::SessionNotCreated, err.to_string())
    }
}

/// Provides matching of `moz:firefoxOptions` and resolutionnized  of which Firefox
/// binary to use.
///
/// `FirefoxCapabilities` is constructed with the fallback binary, should
/// `moz:firefoxOptions` not contain a binary entry.  This may either be the
/// system Firefox installation or an override, for example given to the
/// `--binary` flag of geckodriver.
pub struct FirefoxCapabilities<'a> {
    pub chosen_binary: Option<PathBuf>,
    fallback_binary: Option<&'a PathBuf>,
    version_cache: BTreeMap<PathBuf, Result<Version, VersionError>>,
}

impl<'a> FirefoxCapabilities<'a> {
    pub fn new(fallback_binary: Option<&'a PathBuf>) -> FirefoxCapabilities<'a> {
        FirefoxCapabilities {
            chosen_binary: None,
            fallback_binary,
            version_cache: BTreeMap::new(),
        }
    }

    fn set_binary(&mut self, capabilities: &Map<String, Value>) {
        self.chosen_binary = capabilities
            .get("moz:firefoxOptions")
            .and_then(|x| x.get("binary"))
            .and_then(|x| x.as_str())
            .map(PathBuf::from)
            .or_else(|| self.fallback_binary.cloned())
            .or_else(firefox_default_path);
    }

    fn version(&mut self, binary: Option<&Path>) -> Result<Version, VersionError> {
        if let Some(binary) = binary {
            if let Some(cache_value) = self.version_cache.get(binary) {
                return cache_value.clone();
            }
            let rv = self
                .version_from_ini(binary)
                .or_else(|_| self.version_from_binary(binary));
            if let Ok(ref version) = rv {
                debug!("Found version {}", version);
            } else {
                debug!("Failed to get binary version");
            }
            self.version_cache.insert(binary.to_path_buf(), rv.clone());
            rv
        } else {
            Err(VersionError::MissingBinary)
        }
    }

    fn version_from_ini(&self, binary: &Path) -> Result<Version, VersionError> {
        debug!("Trying to read firefox version from ini files");
        let version = firefox_version(binary)?;
        if let Some(version_string) = version.version_string {
            Version::from_str(&version_string).map_err(|err| err.into())
        } else {
            Err(VersionError::VersionError(
                mozversion::Error::MetadataError("Missing version string".into()),
            ))
        }
    }

    fn version_from_binary(&self, binary: &Path) -> Result<Version, VersionError> {
        debug!("Trying to read firefox version from binary");
        Ok(firefox_binary_version(binary)?)
    }
}

impl BrowserCapabilities for FirefoxCapabilities<'_> {
    fn init(&mut self, capabilities: &Capabilities) {
        self.set_binary(capabilities);
    }

    fn browser_name(&mut self, _: &Capabilities) -> WebDriverResult<Option<String>> {
        Ok(Some("firefox".into()))
    }

    fn browser_version(&mut self, _: &Capabilities) -> WebDriverResult<Option<String>> {
        let binary = self.chosen_binary.clone();
        self.version(binary.as_ref().map(|x| x.as_ref()))
            .map_err(|err| err.into())
            .map(|x| Some(x.to_string()))
    }

    fn platform_name(&mut self, _: &Capabilities) -> WebDriverResult<Option<String>> {
        Ok(if cfg!(target_os = "windows") {
            Some("windows".into())
        } else if cfg!(target_os = "macos") {
            Some("mac".into())
        } else if cfg!(target_os = "linux") {
            Some("linux".into())
        } else {
            None
        })
    }

    fn accept_insecure_certs(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(true)
    }

    fn accept_proxy(&mut self, _: &Capabilities, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(true)
    }

    fn set_window_rect(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(true)
    }

    fn compare_browser_version(
        &mut self,
        version: &str,
        comparison: &str,
    ) -> WebDriverResult<bool> {
        Version::from_str(version)
            .map_err(VersionError::from)?
            .matches(comparison)
            .map_err(|err| VersionError::from(err).into())
    }

    fn strict_file_interactability(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(true)
    }

    fn web_socket_url(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(true)
    }

    fn webauthn_virtual_authenticators(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(true)
    }

    fn webauthn_extension_uvm(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(false)
    }

    fn webauthn_extension_prf(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(false)
    }

    fn webauthn_extension_large_blob(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(false)
    }

    fn webauthn_extension_cred_blob(&mut self, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(false)
    }

    fn validate_custom(&mut self, name: &str, value: &Value) -> WebDriverResult<()> {
        if !name.starts_with("moz:") {
            return Ok(());
        }
        match name {
            "moz:firefoxOptions" => {
                let data = try_opt!(
                    value.as_object(),
                    ErrorStatus::InvalidArgument,
                    "moz:firefoxOptions is not an object"
                );
                for (key, value) in data.iter() {
                    match &**key {
                        "androidActivity"
                        | "androidDeviceSerial"
                        | "androidPackage"
                        | "profile" => {
                            if !value.is_string() {
                                return Err(WebDriverError::new(
                                    ErrorStatus::InvalidArgument,
                                    format!("{} is not a string", &**key),
                                ));
                            }
                        }
                        "androidIntentArguments" | "args" => {
                            if !try_opt!(
                                value.as_array(),
                                ErrorStatus::InvalidArgument,
                                format!("{} is not an array", &**key)
                            )
                            .iter()
                            .all(|value| value.is_string())
                            {
                                return Err(WebDriverError::new(
                                    ErrorStatus::InvalidArgument,
                                    format!("{} entry is not a string", &**key),
                                ));
                            }
                        }
                        "binary" => {
                            if let Some(binary) = value.as_str() {
                                if !data.contains_key("androidPackage")
                                    && self.version(Some(Path::new(binary))).is_err()
                                {
                                    return Err(WebDriverError::new(
                                        ErrorStatus::InvalidArgument,
                                        format!("{} is not a Firefox executable", &**key),
                                    ));
                                }
                            } else {
                                return Err(WebDriverError::new(
                                    ErrorStatus::InvalidArgument,
                                    format!("{} is not a string", &**key),
                                ));
                            }
                        }
                        "env" => {
                            let env_data = try_opt!(
                                value.as_object(),
                                ErrorStatus::InvalidArgument,
                                "env value is not an object"
                            );
                            if !env_data.values().all(Value::is_string) {
                                return Err(WebDriverError::new(
                                    ErrorStatus::InvalidArgument,
                                    "Environment values were not all strings",
                                ));
                            }
                        }
                        "log" => {
                            let log_data = try_opt!(
                                value.as_object(),
                                ErrorStatus::InvalidArgument,
                                "log value is not an object"
                            );
                            for (log_key, log_value) in log_data.iter() {
                                match &**log_key {
                                    "level" => {
                                        let level = try_opt!(
                                            log_value.as_str(),
                                            ErrorStatus::InvalidArgument,
                                            "log level is not a string"
                                        );
                                        if Level::from_str(level).is_err() {
                                            return Err(WebDriverError::new(
                                                ErrorStatus::InvalidArgument,
                                                format!("Not a valid log level: {}", level),
                                            ));
                                        }
                                    }
                                    x => {
                                        return Err(WebDriverError::new(
                                            ErrorStatus::InvalidArgument,
                                            format!("Invalid log field {}", x),
                                        ))
                                    }
                                }
                            }
                        }
                        "prefs" => {
                            let prefs_data = try_opt!(
                                value.as_object(),
                                ErrorStatus::InvalidArgument,
                                "prefs value is not an object"
                            );
                            let is_pref_value_type = |x: &Value| {
                                x.is_string() || x.is_i64() || x.is_u64() || x.is_boolean()
                            };
                            if !prefs_data.values().all(is_pref_value_type) {
                                return Err(WebDriverError::new(
                                    ErrorStatus::InvalidArgument,
                                    "Preference values not all string or integer or boolean",
                                ));
                            }
                        }
                        x => {
                            return Err(WebDriverError::new(
                                ErrorStatus::InvalidArgument,
                                format!("Invalid moz:firefoxOptions field {}", x),
                            ))
                        }
                    }
                }
            }
            "moz:webdriverClick" => {
                if !value.is_boolean() {
                    return Err(WebDriverError::new(
                        ErrorStatus::InvalidArgument,
                        "moz:webdriverClick is not a boolean",
                    ));
                }
            }
            // Bug 1967916: Remove when Firefox 140 is no longer supported
            "moz:debuggerAddress" => {
                if !value.is_boolean() {
                    return Err(WebDriverError::new(
                        ErrorStatus::InvalidArgument,
                        "moz:debuggerAddress is not a boolean",
                    ));
                }
            }
            _ => {
                return Err(WebDriverError::new(
                    ErrorStatus::InvalidArgument,
                    format!("Unrecognised option {}", name),
                ))
            }
        }
        Ok(())
    }

    fn accept_custom(&mut self, _: &str, _: &Value, _: &Capabilities) -> WebDriverResult<bool> {
        Ok(true)
    }
}

/// Android-specific options in the `moz:firefoxOptions` struct.
/// These map to "androidCamelCase", following [chromedriver's Android-specific
/// Capabilities](http://chromedriver.chromium.org/getting-started/getting-started---android).
#[derive(Default, Clone, Debug, PartialEq)]
pub struct AndroidOptions {
    pub activity: Option<String>,
    pub device_serial: Option<String>,
    pub intent_arguments: Option<Vec<String>>,
    pub package: String,
    pub storage: AndroidStorageInput,
}

impl AndroidOptions {
    pub fn new(package: String, storage: AndroidStorageInput) -> AndroidOptions {
        AndroidOptions {
            package,
            storage,
            ..Default::default()
        }
    }
}

#[derive(Debug, Default, PartialEq)]
pub enum ProfileType {
    Path(Profile),
    Named,
    #[default]
    Temporary,
}

/// Rust representation of `moz:firefoxOptions`.
///
/// Calling `FirefoxOptions::from_capabilities(binary, capabilities)` causes
/// the encoded profile, the binary arguments, log settings, and additional
/// preferences to be checked and unmarshaled from the `moz:firefoxOptions`
/// JSON Object into a Rust representation.
#[derive(Default, Debug)]
pub struct FirefoxOptions {
    pub binary: Option<PathBuf>,
    pub profile: ProfileType,
    pub args: Option<Vec<String>>,
    pub env: Option<Vec<(String, String)>>,
    pub log: LogOptions,
    pub prefs: Vec<(String, Pref)>,
    pub android: Option<AndroidOptions>,
    pub use_websocket: bool,
}

impl FirefoxOptions {
    pub fn new() -> FirefoxOptions {
        Default::default()
    }

    pub(crate) fn from_capabilities(
        binary_path: Option<PathBuf>,
        settings: &MarionetteSettings,
        matched: &mut Capabilities,
    ) -> WebDriverResult<FirefoxOptions> {
        let mut rv = FirefoxOptions::new();
        rv.binary = binary_path;

        if let Some(json) = matched.remove("moz:firefoxOptions") {
            let options = json.as_object().ok_or_else(|| {
                WebDriverError::new(
                    ErrorStatus::InvalidArgument,
                    "'moz:firefoxOptions' \
                 capability is not an object",
                )
            })?;

            if options.get("androidPackage").is_some() && options.get("binary").is_some() {
                return Err(WebDriverError::new(
                    ErrorStatus::InvalidArgument,
                    "androidPackage and binary are mutual exclusive",
                ));
            }

            rv.android = FirefoxOptions::load_android(settings.android_storage, options)?;
            rv.args = FirefoxOptions::load_args(options)?;
            rv.env = FirefoxOptions::load_env(options)?;
            rv.log = FirefoxOptions::load_log(options)?;
            rv.prefs = FirefoxOptions::load_prefs(options)?;
            if let Some(profile) =
                FirefoxOptions::load_profile(settings.profile_root.as_deref(), options)?
            {
                rv.profile = ProfileType::Path(profile);
            }
        }

        if let Some(args) = rv.args.as_ref() {
            let os_args = parse_args(args.iter().map(OsString::from).collect::<Vec<_>>().iter());

            if let Some(path) = get_arg_value(os_args.iter(), Arg::Profile) {
                if let ProfileType::Path(_) = rv.profile {
                    return Err(WebDriverError::new(
                        ErrorStatus::InvalidArgument,
                        "Can't provide both a --profile argument and a profile",
                    ));
                }
                let path_buf = PathBuf::from(path);
                rv.profile = ProfileType::Path(Profile::new_from_path(&path_buf)?);
            }

            if get_arg_value(os_args.iter(), Arg::NamedProfile).is_some() {
                if let ProfileType::Path(_) = rv.profile {
                    return Err(WebDriverError::new(
                        ErrorStatus::InvalidArgument,
                        "Can't provide both a -P argument and a profile",
                    ));
                }
                // See bug 1757720
                warn!("Firefox was configured to use a named profile (`-P <name>`). \
                       Support for named profiles will be removed in a future geckodriver release. \
                       Please instead use the `--profile <path>` Firefox argument to start with an existing profile");
                rv.profile = ProfileType::Named;
            }

            // Block these Firefox command line arguments that should not be settable
            // via session capabilities.
            if let Some(arg) = os_args
                .iter()
                .filter_map(|(opt_arg, _)| opt_arg.as_ref())
                .find(|arg| {
                    matches!(
                        arg,
                        Arg::Marionette
                            | Arg::RemoteAllowHosts
                            | Arg::RemoteAllowOrigins
                            | Arg::RemoteDebuggingPort
                    )
                })
            {
                return Err(WebDriverError::new(
                    ErrorStatus::InvalidArgument,
                    format!("Argument {} can't be set via capabilities", arg),
                ));
            };
        }

        let has_web_socket_url = matched
            .get("webSocketUrl")
            .and_then(|x| x.as_bool())
            .unwrap_or(false);

        let has_debugger_address = matched
            .remove("moz:debuggerAddress")
            .and_then(|x| x.as_bool())
            .unwrap_or(false);

        // Set a command line provided port for the Remote Agent for now.
        // It needs to be the same on the host and the Android device.
        if has_web_socket_url || has_debugger_address {
            rv.use_websocket = true;

            // Bug 1722863: Setting of command line arguments would be
            // better suited in the individual Browser implementations.
            let mut remote_args = Vec::new();
            remote_args.push("--remote-debugging-port".to_owned());
            remote_args.push(settings.websocket_port.to_string());

            // Handle additional hosts for WebDriver BiDi WebSocket connections
            if !settings.allow_hosts.is_empty() {
                remote_args.push("--remote-allow-hosts".to_owned());
                remote_args.push(
                    settings
                        .allow_hosts
                        .iter()
                        .map(|host| host.to_string())
                        .collect::<Vec<String>>()
                        .join(","),
                );
            }

            // Handle additional origins for WebDriver BiDi WebSocket connections
            if !settings.allow_origins.is_empty() {
                remote_args.push("--remote-allow-origins".to_owned());
                remote_args.push(
                    settings
                        .allow_origins
                        .iter()
                        .map(|origin| origin.to_string())
                        .collect::<Vec<String>>()
                        .join(","),
                );
            }

            if let Some(ref mut args) = rv.args {
                args.append(&mut remote_args);
            } else {
                rv.args = Some(remote_args);
            }
        }

        Ok(rv)
    }

    fn load_profile(
        profile_root: Option<&Path>,
        options: &Capabilities,
    ) -> WebDriverResult<Option<Profile>> {
        if let Some(profile_json) = options.get("profile") {
            let profile_base64 = profile_json.as_str().ok_or_else(|| {
                WebDriverError::new(ErrorStatus::InvalidArgument, "Profile is not a string")
            })?;
            let profile_zip = &*BASE64_STANDARD.decode(profile_base64)?;

            // Create an emtpy profile directory
            let profile = Profile::new(profile_root)?;
            unzip_buffer(
                profile_zip,
                profile
                    .temp_dir
                    .as_ref()
                    .expect("Profile doesn't have a path")
                    .path(),
            )?;

            Ok(Some(profile))
        } else {
            Ok(None)
        }
    }

    fn load_args(options: &Capabilities) -> WebDriverResult<Option<Vec<String>>> {
        if let Some(args_json) = options.get("args") {
            let args_array = args_json.as_array().ok_or_else(|| {
                WebDriverError::new(ErrorStatus::InvalidArgument, "Arguments were not an array")
            })?;
            let args = args_array
                .iter()
                .map(|x| x.as_str().map(|x| x.to_owned()))
                .collect::<Option<Vec<String>>>()
                .ok_or_else(|| {
                    WebDriverError::new(
                        ErrorStatus::InvalidArgument,
                        "Arguments entries were not all strings",
                    )
                })?;

            Ok(Some(args))
        } else {
            Ok(None)
        }
    }

    pub fn load_env(options: &Capabilities) -> WebDriverResult<Option<Vec<(String, String)>>> {
        if let Some(env_data) = options.get("env") {
            let env = env_data.as_object().ok_or_else(|| {
                WebDriverError::new(ErrorStatus::InvalidArgument, "Env was not an object")
            })?;
            let mut rv = Vec::with_capacity(env.len());
            for (key, value) in env.iter() {
                rv.push((
                    key.clone(),
                    value
                        .as_str()
                        .ok_or_else(|| {
                            WebDriverError::new(
                                ErrorStatus::InvalidArgument,
                                "Env value is not a string",
                            )
                        })?
                        .to_string(),
                ));
            }
            Ok(Some(rv))
        } else {
            Ok(None)
        }
    }

    fn load_log(options: &Capabilities) -> WebDriverResult<LogOptions> {
        if let Some(json) = options.get("log") {
            let log = json.as_object().ok_or_else(|| {
                WebDriverError::new(ErrorStatus::InvalidArgument, "Log section is not an object")
            })?;

            let level = match log.get("level") {
                Some(json) => {
                    let s = json.as_str().ok_or_else(|| {
                        WebDriverError::new(
                            ErrorStatus::InvalidArgument,
                            "Log level is not a string",
                        )
                    })?;
                    Some(Level::from_str(s).ok().ok_or_else(|| {
                        WebDriverError::new(ErrorStatus::InvalidArgument, "Log level is unknown")
                    })?)
                }
                None => None,
            };

            Ok(LogOptions { level })
        } else {
            Ok(Default::default())
        }
    }

    pub fn load_prefs(options: &Capabilities) -> WebDriverResult<Vec<(String, Pref)>> {
        if let Some(prefs_data) = options.get("prefs") {
            let prefs = prefs_data.as_object().ok_or_else(|| {
                WebDriverError::new(ErrorStatus::InvalidArgument, "Prefs were not an object")
            })?;
            let mut rv = Vec::with_capacity(prefs.len());
            for (key, value) in prefs.iter() {
                rv.push((key.clone(), pref_from_json(value)?));
            }
            Ok(rv)
        } else {
            Ok(vec![])
        }
    }

    pub fn load_android(
        storage: AndroidStorageInput,
        options: &Capabilities,
    ) -> WebDriverResult<Option<AndroidOptions>> {
        if let Some(package_json) = options.get("androidPackage") {
            let package = package_json
                .as_str()
                .ok_or_else(|| {
                    WebDriverError::new(
                        ErrorStatus::InvalidArgument,
                        "androidPackage is not a string",
                    )
                })?
                .to_owned();

            // https://developer.android.com/studio/build/application-id
            let package_regexp =
                Regex::new(r"^([a-zA-Z][a-zA-Z0-9_]*\.){1,}([a-zA-Z][a-zA-Z0-9_]*)$").unwrap();
            if !package_regexp.is_match(package.as_bytes()) {
                return Err(WebDriverError::new(
                    ErrorStatus::InvalidArgument,
                    "Not a valid androidPackage name",
                ));
            }

            let mut android = AndroidOptions::new(package.clone(), storage);

            android.activity = match options.get("androidActivity") {
                Some(json) => {
                    let activity = json
                        .as_str()
                        .ok_or_else(|| {
                            WebDriverError::new(
                                ErrorStatus::InvalidArgument,
                                "androidActivity is not a string",
                            )
                        })?
                        .to_owned();

                    if activity.contains('/') {
                        return Err(WebDriverError::new(
                            ErrorStatus::InvalidArgument,
                            "androidActivity should not contain '/",
                        ));
                    }

                    Some(activity)
                }
                None => {
                    match package.as_str() {
                        "org.mozilla.firefox"
                        | "org.mozilla.firefox_beta"
                        | "org.mozilla.fenix"
                        | "org.mozilla.fenix.debug"
                        | "org.mozilla.reference.browser" => {
                            Some("org.mozilla.fenix.IntentReceiverActivity".to_string())
                        }
                        "org.mozilla.focus"
                        | "org.mozilla.focus.debug"
                        | "org.mozilla.klar"
                        | "org.mozilla.klar.debug" => {
                            Some("org.mozilla.focus.activity.IntentReceiverActivity".to_string())
                        }
                        // For all other applications fallback to auto-detection.
                        _ => None,
                    }
                }
            };

            android.device_serial = match options.get("androidDeviceSerial") {
                Some(json) => Some(
                    json.as_str()
                        .ok_or_else(|| {
                            WebDriverError::new(
                                ErrorStatus::InvalidArgument,
                                "androidDeviceSerial is not a string",
                            )
                        })?
                        .to_owned(),
                ),
                None => None,
            };

            android.intent_arguments = match options.get("androidIntentArguments") {
                Some(json) => {
                    let args_array = json.as_array().ok_or_else(|| {
                        WebDriverError::new(
                            ErrorStatus::InvalidArgument,
                            "androidIntentArguments is not an array",
                        )
                    })?;
                    let args = args_array
                        .iter()
                        .map(|x| x.as_str().map(|x| x.to_owned()))
                        .collect::<Option<Vec<String>>>()
                        .ok_or_else(|| {
                            WebDriverError::new(
                                ErrorStatus::InvalidArgument,
                                "androidIntentArguments entries are not all strings",
                            )
                        })?;

                    Some(args)
                }
                None => {
                    // All GeckoView based applications support this view,
                    // and allow to open a blank page in a Gecko window.
                    Some(vec![
                        "-a".to_string(),
                        "android.intent.action.VIEW".to_string(),
                        "-d".to_string(),
                        "about:blank".to_string(),
                    ])
                }
            };

            Ok(Some(android))
        } else {
            Ok(None)
        }
    }
}

fn pref_from_json(value: &Value) -> WebDriverResult<Pref> {
    match *value {
        Value::String(ref x) => Ok(Pref::new(x.clone())),
        Value::Number(ref x) => Ok(Pref::new(x.as_i64().unwrap())),
        Value::Bool(x) => Ok(Pref::new(x)),
        _ => Err(WebDriverError::new(
            ErrorStatus::UnknownError,
            "Could not convert pref value to string, boolean, or integer",
        )),
    }
}

fn unzip_buffer(buf: &[u8], dest_dir: &Path) -> WebDriverResult<()> {
    let reader = Cursor::new(buf);
    let mut zip = zip::ZipArchive::new(reader)
        .map_err(|_| WebDriverError::new(ErrorStatus::UnknownError, "Failed to unzip profile"))?;

    for i in 0..zip.len() {
        let mut file = zip.by_index(i).map_err(|_| {
            WebDriverError::new(
                ErrorStatus::UnknownError,
                "Processing profile zip file failed",
            )
        })?;
        let unzip_path = {
            let name = file.name();
            let is_dir = name.ends_with('/');
            let rel_path = Path::new(name);
            let dest_path = dest_dir.join(rel_path);

            {
                let create_dir = if is_dir {
                    Some(dest_path.as_path())
                } else {
                    dest_path.parent()
                };
                if let Some(dir) = create_dir {
                    if !dir.exists() {
                        debug!("Creating profile directory tree {}", dir.to_string_lossy());
                        fs::create_dir_all(dir)?;
                    }
                }
            }

            if is_dir {
                None
            } else {
                Some(dest_path)
            }
        };

        if let Some(unzip_path) = unzip_path {
            debug!("Extracting profile to {}", unzip_path.to_string_lossy());
            let dest = fs::File::create(unzip_path)?;
            if file.size() > 0 {
                let mut writer = BufWriter::new(dest);
                io::copy(&mut file, &mut writer)?;
            }
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    extern crate mozprofile;

    use self::mozprofile::preferences::Pref;
    use super::*;
    use serde_json::{json, Map, Value};
    use std::fs::File;
    use std::io::Read;
    use url::{Host, Url};
    use webdriver::capabilities::Capabilities;

    fn example_profile() -> Value {
        let mut profile_data = Vec::with_capacity(1024);
        let mut profile = File::open("src/tests/profile.zip").unwrap();
        profile.read_to_end(&mut profile_data).unwrap();
        Value::String(BASE64_STANDARD.encode(&profile_data))
    }

    fn make_options(
        firefox_opts: Capabilities,
        marionette_settings: Option<MarionetteSettings>,
    ) -> WebDriverResult<FirefoxOptions> {
        let mut caps = Capabilities::new();
        caps.insert("moz:firefoxOptions".into(), Value::Object(firefox_opts));

        FirefoxOptions::from_capabilities(None, &marionette_settings.unwrap_or_default(), &mut caps)
    }

    #[test]
    fn fx_options_default() {
        let opts: FirefoxOptions = Default::default();
        assert_eq!(opts.android, None);
        assert_eq!(opts.args, None);
        assert_eq!(opts.binary, None);
        assert_eq!(opts.log, LogOptions { level: None });
        assert_eq!(opts.prefs, vec![]);
        // Profile doesn't support PartialEq
        // assert_eq!(opts.profile, None);
    }

    #[test]
    fn fx_options_from_capabilities_no_binary_and_empty_caps() {
        let mut caps = Capabilities::new();

        let marionette_settings = Default::default();
        let opts = FirefoxOptions::from_capabilities(None, &marionette_settings, &mut caps)
            .expect("valid firefox options");
        assert_eq!(opts.android, None);
        assert_eq!(opts.args, None);
        assert_eq!(opts.binary, None);
        assert_eq!(opts.log, LogOptions { level: None });
        assert_eq!(opts.prefs, vec![]);
    }

    #[test]
    fn fx_options_from_capabilities_with_binary_and_caps() {
        let mut caps = Capabilities::new();
        caps.insert(
            "moz:firefoxOptions".into(),
            Value::Object(Capabilities::new()),
        );

        let binary = PathBuf::from("foo");
        let marionette_settings = Default::default();

        let opts = FirefoxOptions::from_capabilities(
            Some(binary.clone()),
            &marionette_settings,
            &mut caps,
        )
        .expect("valid firefox options");
        assert_eq!(opts.android, None);
        assert_eq!(opts.args, None);
        assert_eq!(opts.binary, Some(binary));
        assert_eq!(opts.log, LogOptions { level: None });
        assert_eq!(opts.prefs, vec![]);
    }

    #[test]
    fn fx_options_from_capabilities_with_blocked_firefox_arguments() {
        let blocked_args = vec![
            "--marionette",
            "--remote-allow-hosts",
            "--remote-allow-origins",
            "--remote-debugging-port",
        ];

        for arg in blocked_args {
            let mut firefox_opts = Capabilities::new();
            firefox_opts.insert("args".into(), json!([arg]));

            make_options(firefox_opts, None).expect_err("invalid firefox options");
        }
    }

    #[test]
    fn fx_options_from_capabilities_with_websocket_url_not_set() {
        let mut caps = Capabilities::new();

        let marionette_settings = Default::default();
        let opts = FirefoxOptions::from_capabilities(None, &marionette_settings, &mut caps)
            .expect("Valid Firefox options");

        assert!(
            opts.args.is_none(),
            "CLI arguments for Firefox unexpectedly found"
        );
    }

    #[test]
    fn fx_options_from_capabilities_with_websocket_url_false() {
        let mut caps = Capabilities::new();
        caps.insert("webSocketUrl".into(), json!(false));

        let marionette_settings = Default::default();
        let opts = FirefoxOptions::from_capabilities(None, &marionette_settings, &mut caps)
            .expect("Valid Firefox options");

        assert!(
            opts.args.is_none(),
            "CLI arguments for Firefox unexpectedly found"
        );
    }

    #[test]
    fn fx_options_from_capabilities_with_websocket_url_true() {
        let mut caps = Capabilities::new();
        caps.insert("webSocketUrl".into(), json!(true));

        let settings = MarionetteSettings {
            websocket_port: 1234,
            ..Default::default()
        };
        let opts = FirefoxOptions::from_capabilities(None, &settings, &mut caps)
            .expect("Valid Firefox options");

        if let Some(args) = opts.args {
            let mut iter = args.iter();
            assert!(iter.any(|arg| arg == &"--remote-debugging-port".to_owned()));
            assert_eq!(iter.next(), Some(&"1234".to_owned()));
        } else {
            panic!("CLI arguments for Firefox not found");
        }
    }

    #[test]
    fn fx_options_from_capabilities_with_websocket_and_allow_hosts() {
        let mut caps = Capabilities::new();
        caps.insert("webSocketUrl".into(), json!(true));

        let mut marionette_settings: MarionetteSettings = Default::default();
        marionette_settings.allow_hosts = vec![
            Host::parse("foo").expect("host"),
            Host::parse("bar").expect("host"),
        ];
        let opts = FirefoxOptions::from_capabilities(None, &marionette_settings, &mut caps)
            .expect("Valid Firefox options");

        if let Some(args) = opts.args {
            let mut iter = args.iter();
            assert!(iter.any(|arg| arg == &"--remote-allow-hosts".to_owned()));
            assert_eq!(iter.next(), Some(&"foo,bar".to_owned()));
            assert!(!iter.any(|arg| arg == &"--remote-allow-origins".to_owned()));
        } else {
            panic!("CLI arguments for Firefox not found");
        }
    }

    #[test]
    fn fx_options_from_capabilities_with_websocket_and_allow_origins() {
        let mut caps = Capabilities::new();
        caps.insert("webSocketUrl".into(), json!(true));

        let mut marionette_settings: MarionetteSettings = Default::default();
        marionette_settings.allow_origins = vec![
            Url::parse("http://foo/").expect("url"),
            Url::parse("http://bar/").expect("url"),
        ];
        let opts = FirefoxOptions::from_capabilities(None, &marionette_settings, &mut caps)
            .expect("Valid Firefox options");

        if let Some(args) = opts.args {
            let mut iter = args.iter();
            assert!(iter.any(|arg| arg == &"--remote-allow-origins".to_owned()));
            assert_eq!(iter.next(), Some(&"http://foo/,http://bar/".to_owned()));
            assert!(!iter.any(|arg| arg == &"--remote-allow-hosts".to_owned()));
        } else {
            panic!("CLI arguments for Firefox not found");
        }
    }

    #[test]
    fn fx_options_from_capabilities_with_debugger_address_not_set() {
        let caps = Capabilities::new();

        let opts = make_options(caps, None).expect("valid firefox options");
        assert!(
            opts.args.is_none(),
            "CLI arguments for Firefox unexpectedly found"
        );
    }

    #[test]
    fn fx_options_from_capabilities_with_debugger_address_false() {
        let mut caps = Capabilities::new();
        caps.insert("moz:debuggerAddress".into(), json!(false));

        let opts = make_options(caps, None).expect("valid firefox options");
        assert!(
            opts.args.is_none(),
            "CLI arguments for Firefox unexpectedly found"
        );
    }

    #[test]
    fn fx_options_from_capabilities_with_debugger_address_true() {
        let mut caps = Capabilities::new();
        caps.insert("moz:debuggerAddress".into(), json!(true));

        let settings = MarionetteSettings {
            websocket_port: 1234,
            ..Default::default()
        };
        let opts = FirefoxOptions::from_capabilities(None, &settings, &mut caps)
            .expect("Valid Firefox options");

        if let Some(args) = opts.args {
            let mut iter = args.iter();
            assert!(iter.any(|arg| arg == &"--remote-debugging-port".to_owned()));
            assert_eq!(iter.next(), Some(&"1234".to_owned()));
        } else {
            panic!("CLI arguments for Firefox not found");
        }
    }

    #[test]
    fn fx_options_from_capabilities_with_invalid_caps() {
        let mut caps = Capabilities::new();
        caps.insert("moz:firefoxOptions".into(), json!(42));

        let marionette_settings = Default::default();
        FirefoxOptions::from_capabilities(None, &marionette_settings, &mut caps)
            .expect_err("Firefox options need to be of type object");
    }

    #[test]
    fn fx_options_android_package_and_binary() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo"));
        firefox_opts.insert("binary".into(), json!("bar"));

        make_options(firefox_opts, None)
            .expect_err("androidPackage and binary are mutual exclusive");
    }

    #[test]
    fn fx_options_android_no_package() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidAvtivity".into(), json!("foo"));

        let opts = make_options(firefox_opts, None).expect("valid firefox options");
        assert_eq!(opts.android, None);
    }

    #[test]
    fn fx_options_android_package_valid_value() {
        for value in ["foo.bar", "foo.bar.cheese.is.good", "Foo.Bar_9"].iter() {
            let mut firefox_opts = Capabilities::new();
            firefox_opts.insert("androidPackage".into(), json!(value));

            let opts = make_options(firefox_opts, None).expect("valid firefox options");
            assert_eq!(opts.android.unwrap().package, value.to_string());
        }
    }

    #[test]
    fn fx_options_android_package_invalid_type() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!(42));

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn fx_options_android_package_invalid_value() {
        for value in ["../foo", "\\foo\n", "foo", "_foo", "0foo"].iter() {
            let mut firefox_opts = Capabilities::new();
            firefox_opts.insert("androidPackage".into(), json!(value));
            make_options(firefox_opts, None).expect_err("invalid firefox options");
        }
    }

    #[test]
    fn fx_options_android_activity_default_known_apps() {
        let packages = vec![
            "org.mozilla.firefox",
            "org.mozilla.firefox_beta",
            "org.mozilla.fenix",
            "org.mozilla.fenix.debug",
            "org.mozilla.focus",
            "org.mozilla.focus.debug",
            "org.mozilla.klar",
            "org.mozilla.klar.debug",
            "org.mozilla.reference.browser",
        ];

        for package in packages {
            let mut firefox_opts = Capabilities::new();
            firefox_opts.insert("androidPackage".into(), json!(package));

            let opts = make_options(firefox_opts, None).expect("valid firefox options");
            assert!(opts
                .android
                .unwrap()
                .activity
                .unwrap()
                .contains("IntentReceiverActivity"));
        }
    }

    #[test]
    fn fx_options_android_activity_default_unknown_apps() {
        let packages = vec!["org.mozilla.geckoview_example", "com.some.other.app"];

        for package in packages {
            let mut firefox_opts = Capabilities::new();
            firefox_opts.insert("androidPackage".into(), json!(package));

            let opts = make_options(firefox_opts, None).expect("valid firefox options");
            assert_eq!(opts.android.unwrap().activity, None);
        }

        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert(
            "androidPackage".into(),
            json!("org.mozilla.geckoview_example"),
        );

        let opts = make_options(firefox_opts, None).expect("valid firefox options");
        assert_eq!(opts.android.unwrap().activity, None);
    }

    #[test]
    fn fx_options_android_activity_override() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidActivity".into(), json!("foo"));

        let opts = make_options(firefox_opts, None).expect("valid firefox options");
        assert_eq!(opts.android.unwrap().activity, Some("foo".to_string()));
    }

    #[test]
    fn fx_options_android_activity_invalid_type() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidActivity".into(), json!(42));

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn fx_options_android_activity_invalid_value() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidActivity".into(), json!("foo.bar/cheese"));

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn fx_options_android_device_serial() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidDeviceSerial".into(), json!("cheese"));

        let opts = make_options(firefox_opts, None).expect("valid firefox options");
        assert_eq!(
            opts.android.unwrap().device_serial,
            Some("cheese".to_string())
        );
    }

    #[test]
    fn fx_options_android_device_serial_invalid() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidDeviceSerial".into(), json!(42));

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn fx_options_android_intent_arguments_defaults() {
        let packages = vec![
            "org.mozilla.firefox",
            "org.mozilla.firefox_beta",
            "org.mozilla.fenix",
            "org.mozilla.fenix.debug",
            "org.mozilla.geckoview_example",
            "org.mozilla.reference.browser",
            "com.some.other.app",
        ];

        for package in packages {
            let mut firefox_opts = Capabilities::new();
            firefox_opts.insert("androidPackage".into(), json!(package));

            let opts = make_options(firefox_opts, None).expect("valid firefox options");
            assert_eq!(
                opts.android.unwrap().intent_arguments,
                Some(vec![
                    "-a".to_string(),
                    "android.intent.action.VIEW".to_string(),
                    "-d".to_string(),
                    "about:blank".to_string(),
                ])
            );
        }
    }

    #[test]
    fn fx_options_android_intent_arguments_override() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidIntentArguments".into(), json!(["lorem", "ipsum"]));

        let opts = make_options(firefox_opts, None).expect("valid firefox options");
        assert_eq!(
            opts.android.unwrap().intent_arguments,
            Some(vec!["lorem".to_string(), "ipsum".to_string()])
        );
    }

    #[test]
    fn fx_options_android_intent_arguments_no_array() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidIntentArguments".into(), json!(42));

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn fx_options_android_intent_arguments_invalid_value() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("androidPackage".into(), json!("foo.bar"));
        firefox_opts.insert("androidIntentArguments".into(), json!(["lorem", 42]));

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn fx_options_env() {
        let mut env: Map<String, Value> = Map::new();
        env.insert("TEST_KEY_A".into(), Value::String("test_value_a".into()));
        env.insert("TEST_KEY_B".into(), Value::String("test_value_b".into()));

        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("env".into(), env.into());

        let mut opts = make_options(firefox_opts, None).expect("valid firefox options");
        for sorted in opts.env.iter_mut() {
            sorted.sort()
        }
        assert_eq!(
            opts.env,
            Some(vec![
                ("TEST_KEY_A".into(), "test_value_a".into()),
                ("TEST_KEY_B".into(), "test_value_b".into()),
            ])
        );
    }

    #[test]
    fn fx_options_env_invalid_container() {
        let env = Value::Number(1.into());

        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("env".into(), env);

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn fx_options_env_invalid_value() {
        let mut env: Map<String, Value> = Map::new();
        env.insert("TEST_KEY".into(), Value::Number(1.into()));

        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("env".into(), env.into());

        make_options(firefox_opts, None).expect_err("invalid firefox options");
    }

    #[test]
    fn test_profile() {
        let encoded_profile = example_profile();
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("profile".into(), encoded_profile);

        let opts = make_options(firefox_opts, None).expect("valid firefox options");
        let mut profile = match opts.profile {
            ProfileType::Path(profile) => profile,
            _ => panic!("Expected ProfileType::Path"),
        };
        let prefs = profile.user_prefs().expect("valid preferences");

        println!("{:#?}", prefs.prefs);

        assert_eq!(
            prefs.get("startup.homepage_welcome_url"),
            Some(&Pref::new("data:text/html,PASS"))
        );
    }

    #[test]
    fn fx_options_args_profile() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("args".into(), json!(["--profile", "foo"]));

        let options = make_options(firefox_opts, None).expect("Valid args");
        assert!(matches!(options.profile, ProfileType::Path(_)));
    }

    #[test]
    fn fx_options_args_named_profile() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("args".into(), json!(["-P", "foo"]));

        let options = make_options(firefox_opts, None).expect("Valid args");
        assert!(matches!(options.profile, ProfileType::Named));
    }

    #[test]
    fn fx_options_args_no_profile() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("args".into(), json!(["--headless"]));

        let options = make_options(firefox_opts, None).expect("Valid args");
        assert!(matches!(options.profile, ProfileType::Temporary));
    }

    #[test]
    fn fx_options_args_profile_and_profile() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("args".into(), json!(["--profile", "foo"]));
        firefox_opts.insert("profile".into(), json!("foo"));

        make_options(firefox_opts, None).expect_err("Invalid args");
    }

    #[test]
    fn fx_options_args_p_and_profile() {
        let mut firefox_opts = Capabilities::new();
        firefox_opts.insert("args".into(), json!(["-P"]));
        firefox_opts.insert("profile".into(), json!("foo"));

        make_options(firefox_opts, None).expect_err("Invalid args");
    }
}
