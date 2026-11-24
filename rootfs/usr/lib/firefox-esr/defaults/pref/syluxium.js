// Syluxium defaults for Firefox.
// Use the desktop's own title bar (TDE window frame: move, minimize,
// maximize, close) instead of Firefox drawing tabs + buttons into it.
pref("browser.tabs.inTitlebar", 0);
// Firefox is already the default browser; don't ask.
pref("browser.shell.checkDefaultBrowser", false);
// No first-run welcome / privacy-notice tabs.
pref("browser.aboutwelcome.enabled", false);
pref("datareporting.policy.firstRunURL", "");
pref("toolkit.telemetry.reportingpolicy.firstRun", false);
pref("browser.startup.homepage_override.mstone", "ignore");
// No warning when the system limits unprivileged namespaces (the sandbox then has fewer layers).
pref("security.sandbox.warn_unprivileged_namespaces", false);
