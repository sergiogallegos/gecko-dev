/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 * Bug 1799968 - Build site patch for www.samsung.com on Linux
 * Bug 1860417 - and Android
 * WebCompat issue #108993 - https://webcompat.com/issues/108993
 *
 * Samsung's Watch pages try to detect the OS via navigator.appVersion,
 * but fail with Linux and Android because they expect it to contain the
 * literal string "linux", and their JS breaks.
 *
 * As such this site patch sets appVersion to "5.0 (Linux)", and is
 * only meant to be applied on Linux or Android.
 */

/* globals exportFunction */

console.info(
  "navigator.appVersion has been shimmed for compatibility reasons. See https://webcompat.com/issues/108993 for details."
);

const nav = Object.getPrototypeOf(navigator.wrappedJSObject);
const appVersion = Object.getOwnPropertyDescriptor(nav, "appVersion");
appVersion.get = exportFunction(() => "5.0 (Linux)", window);
Object.defineProperty(nav, "appVersion", appVersion);
