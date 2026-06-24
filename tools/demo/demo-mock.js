/*
 * ESPuino Web-UI demo mock layer
 * ------------------------------------------------------------------
 * Turns the regular management UI into a self-contained GitHub-Pages
 * demo. There is no ESP32 behind it, so this file fakes the device:
 *   - the WebSocket (settings / trackinfo / cover / ping ...)
 *   - the REST endpoints (fetch + XMLHttpRequest)
 *
 * All content is copyright-free: public-domain fairy tales (Brothers
 * Grimm / Andersen) and classical works, plus a few invented titles.
 * Loaded BEFORE the bundled vendor JS so the app never sees a real
 * network. Write operations (save settings, restart, ...) are no-ops
 * that just answer "ok".
 */
(function () {
	"use strict";

	// ---------------------------------------------------------------
	// Demo fixtures
	// ---------------------------------------------------------------

	// File explorer: one array per directory path, shaped exactly like
	// the firmware's /explorer response ({name, dir?} + the root volume
	// label {name, root:"sd"}).
	var FS = {
		"/": [
			{ name: "ESPUINO-SD", root: "sd" },
			{ name: "Hörspiele", dir: true },
			{ name: "Märchen", dir: true },
			{ name: "Klassik", dir: true },
			{ name: "Musik", dir: true },
			{ name: "Playlists", dir: true },
			{ name: "ready.mp3" }
		],
		"/Hörspiele": [
			{ name: "Der Wald der tausend Lichter", dir: true },
			{ name: "Die Robo-Detektive", dir: true },
			{ name: "Sternenstaub und Mondsilber", dir: true }
		],
		"/Hörspiele/Der Wald der tausend Lichter": [
			{ name: "01 - Aufbruch in den Nebel.mp3" },
			{ name: "02 - Die geheime Höhle.mp3" },
			{ name: "03 - Das flüsternde Moos.mp3" },
			{ name: "04 - Heimkehr bei Sonnenaufgang.mp3" }
		],
		"/Hörspiele/Die Robo-Detektive": [
			{ name: "01 - Der verschwundene Schraubenschlüssel.mp3" },
			{ name: "02 - Alarm im Zahnrad-Werk.mp3" },
			{ name: "03 - Das Rätsel der blauen Diode.mp3" }
		],
		"/Hörspiele/Sternenstaub und Mondsilber": [
			{ name: "01 - Eine Reise beginnt.mp3" },
			{ name: "02 - Komet außer Kurs.mp3" }
		],
		"/Märchen": [
			{ name: "Hänsel und Gretel.mp3" },
			{ name: "Rotkäppchen.mp3" },
			{ name: "Die Bremer Stadtmusikanten.mp3" },
			{ name: "Der Froschkönig.mp3" },
			{ name: "Das hässliche Entlein.mp3" },
			{ name: "Die Schildkröte und der Hase.mp3" }
		],
		"/Klassik": [
			{ name: "Vivaldi - Der Frühling.mp3" },
			{ name: "Beethoven - Für Elise.mp3" },
			{ name: "Bach - Air.mp3" },
			{ name: "Mozart - Kleine Nachtmusik.mp3" },
			{ name: "Grieg - Morgenstimmung.mp3" }
		],
		"/Musik": [
			{ name: "Robo-Beats Vol. 1", dir: true },
			{ name: "Sommerwiese.mp3" },
			{ name: "Regentanz.mp3" }
		],
		"/Musik/Robo-Beats Vol. 1": [
			{ name: "01 - Bootsequenz.mp3" },
			{ name: "02 - Servomotor.mp3" },
			{ name: "03 - Nachtschicht.mp3" }
		],
		"/Playlists": [
			{ name: "Gute-Nacht.m3u" },
			{ name: "Auto-Fahrt.m3u" }
		]
	};

	// Saved RFID assignments (flat objects like tagIdToJSON()).
	var RFID = [
		{ id: "0009094506", fileOrUrl: "/Märchen/Hänsel und Gretel.mp3", playMode: 3, lastPlayPos: 142000, trackLastPlayed: 0 },
		{ id: "0006981923", fileOrUrl: "/Klassik", playMode: 5, lastPlayPos: 0, trackLastPlayed: 0 },
		{ id: "0212345678", fileOrUrl: "/Hörspiele/Die Robo-Detektive", playMode: 16, lastPlayPos: 318000, trackLastPlayed: 1 },
		{ id: "0078451200", fileOrUrl: "/Musik/Robo-Beats Vol. 1", playMode: 6, lastPlayPos: 0, trackLastPlayed: 0 },
		{ id: "0033112299", modId: 110 }
	];

	// Full settings object, shaped like Web.cpp::settingsToJSON("").
	function buildSettings() {
		return {
			current: { volume: DEMO_VOLUME, rfidTagId: "0009094506" },
			general: {
				initVolume: 3, maxVolumeSp: 21, maxVolumeHp: 18, sleepInactivity: 10,
				playMono: false, savePosShutdown: false, savePosRfidChge: false,
				savePosPeriodic: true, restartFreshHrs: 24, minResumeSec: 20, seekStep: 300, playLastRfidOnReboot: false, pauseIfRfidRemoved: false,
				stopIfRfidRemoved: false,
				dontAcceptRfidTwice: false, rfidReaderType: 0, pn5180Lpcd: false, slix2Password: "",
				mfrc522Gain: 7, pauseOnMinVol: false, recoverVolBoot: false, volumeCurve: 0,
				readyPath: "/ready.mp3", playStartupSnd: true, noSleepWhenPowered: false,
				poweredVoltage: 3.5, brandText: ""
			},
			equalizer: { gainLowPass: 0, gainBandPass: 0, gainHighPass: 0, profile: "flat" },
			wifi: { hostname: "espuino-demo", scanOnStart: false },
			led: {
				initBrightness: 16, nightBrightness: 2, atmoBrightness: 30, numIndicator: 24,
				numControl: 0, controlLedsEnabled: true, numIdleDots: 4, idleColor: 16777215,
				idleAnimation: 0, offsetPause: false, progColorStart: 65280, progColorEnd: 16711680,
				hueAtmo: 0, satAtmo: 255, dimStates: 3, reverseRot: false, offsetStart: 0
			},
			oled: {
				enable: true, startAnim: 3, showBattery: true, showTime: true, showWifi: true,
				showVolume: true, flip: false, idleLine1: "LEO INDUSTRIES", idleLine2: "AUDIO TERMINAL AT-1"
			},
			buttons: (function () {
				var b = {};
				["0", "1", "2", "3", "4", "5", "6"].forEach(function (i) { b["short" + i] = 0; b["long" + i] = 0; });
				["01", "02", "03", "04", "05", "06", "12", "13", "14", "15", "16", "23", "24", "25", "26", "34", "35", "36", "45", "46", "56"].forEach(function (i) { b["multi" + i] = 0; });
				return b;
			})(),
			rotary: { reverse: false },
			playlist: { sortMode: 0, recDepth: 2 },
			battery: { warnLowVoltage: 3.4, indicatorLow: 3.3, indicatorHi: 4.2, criticalVoltage: 3.1, voltageCheckInterval: 10 },
			ftp: { username: "esp32", password: "esp32", maxUserLength: 31, maxPwdLength: 31, enable: false },
			webdav: { username: "esp32", password: "esp32", enable: false, running: false, port: 80, maxUserLength: 31, maxPwdLength: 31 },
			sync: { url: "", username: "", password: "", abortOnButton: true, deleteRemoved: false, rfidUrl: "", rfidPeers: "", rfidPeerKey: "", rfidLearn: true, backupUrl: "", backupAuto: false },
			mqtt: { enable: false, clientID: "espuino-demo", deviceId: "espuino-demo", baseTopic: "espuino", server: "", username: "", password: "", port: 1883 },
			bluetooth: { deviceName: "ESPuino", pinCode: "" },
			// IR remote: presence of this section reveals the web-UI tab; map = code->command pairs.
			ir: { enabled: true, map: [{ code: 12, cmd: 170 }, { code: 90, cmd: 176 }, { code: 24, cmd: 177 }] }
		};
	}

	// The "currently inserted" playlist (public-domain fairy tales). next/prev
	// step through this list and the title updates accordingly.
	var TRACKS = [
		"Hänsel und Gretel", "Rotkäppchen", "Die Bremer Stadtmusikanten",
		"Der Froschkönig", "Das hässliche Entlein", "Die Schildkröte und der Hase"
	];

	// Now-playing fixture (public-domain audiobook).
	var TRACK = {
		name: TRACKS[2],
		artist: "Gebrüder Grimm", album: "Märchensammlung",
		pausePlay: true, // true = currently paused
		currentTrackNumber: 3, numberOfTracks: TRACKS.length,
		controlsLocked: false, nightMode: false, repeatMode: 0, sleepTimerDuration: 0,
		posPercent: 37, time: 142, duration: 386
	};

	var DEMO_VOLUME = 12;

	function syncTrack() {
		TRACK.name = TRACKS[TRACK.currentTrackNumber - 1] || TRACK.name;
		TRACK.posPercent = TRACK.duration ? Math.round(TRACK.time * 100 / TRACK.duration) : 0;
	}

	var VERSION = { build: "demo (web preview)", upToDate: 1, latest: "demo (web preview)" };

	// Demo banner strings, injected into the locales response under a "demo"
	// namespace so the banner follows the language picked in the UI (the firmware
	// locale files stay untouched).
	var DEMO_I18N = {
		de: { label: "DEMO", text: "Statische Vorschau des ESPuino-Webinterface – kein Gerät verbunden, Aktionen ohne Wirkung.", link: "Projekt auf GitHub" },
		en: { label: "DEMO", text: "Static preview of the ESPuino web interface – no device connected, actions have no effect.", link: "Project on GitHub" },
		fr: { label: "DÉMO", text: "Aperçu statique de l'interface web ESPuino – aucun appareil connecté, les actions n'ont aucun effet.", link: "Projet sur GitHub" }
	};

	// ---------------------------------------------------------------
	// REST routing  (returns {status, contentType, body} or null)
	// ---------------------------------------------------------------
	function jsonResp(obj, status) {
		return { status: status || 200, contentType: "application/json; charset=utf-8", body: JSON.stringify(obj) };
	}

	function route(method, rawUrl) {
		var u;
		try { u = new URL(rawUrl, window.location.href); } catch (e) { return null; }
		var p = u.pathname.replace(/\/+$/, "") || "/";
		var qs = u.searchParams;
		method = (method || "GET").toUpperCase();

		// Locales are real static files shipped next to the page; fetch them for
		// real but tag the language so the demo strings can be merged in.
		var locMatch = p.match(/\/locales\/([a-zA-Z]+)\.json$/);
		if (locMatch) { return { passthrough: "." + u.pathname.substring(u.pathname.indexOf("/locales/")), lng: locMatch[1].toLowerCase() }; }

		// Read endpoints
		if (method === "GET") {
			if (p === "/version") { return jsonResp(VERSION); }
			if (p === "/trackprogress") {
				// advance the playhead while "playing" so the bar moves like on a real device
				if (!TRACK.pausePlay && TRACK.time < TRACK.duration) { TRACK.time = Math.min(TRACK.duration, TRACK.time + 1); }
				syncTrack();
				return jsonResp({ trackProgress: { posPercent: TRACK.posPercent, time: TRACK.time, duration: TRACK.duration } });
			}
			if (p === "/rfid") {
				if (qs.has("ids-only")) { return jsonResp(RFID.map(function (r) { return r.id; })); }
				return jsonResp(RFID);
			}
			if (p === "/explorer") {
				var path = qs.get("path") || "/";
				path = path.replace(/\/+$/, "") || "/";
				return jsonResp(FS[path] || []);
			}
			if (p === "/info") {
				var info = {
					software: { version: "demo (web preview)", build: "demo", git: "demo", arduino: "3.x", idf: "5.x" },
					hardware: { model: "ESP32", revision: 3, freq: 240, readerFirmware: "" },
					memory: { freeHeap: 142336, largestFreeBlock: 110592, freePSRam: 3801088 },
					wifi: { ip: "192.168.1.34", macAddress: "A0:B1:C2:D3:E4:F5", rssi: -58 },
					audio: { firstStart: Math.floor(Date.now() / 1000) - 5184000, playtimeTotal: 486000, playtimeSinceStart: 5400, playToday: 3600, playYesterday: 7200, play7d: 32400, play30d: 129600 },
					sdcard: { size: 30437, free: 21347 }, // MB, like the device (the UI divides by 1024 -> GB)
					battery: { currVoltage: 3.94, chargeLevel: 78 }
				};
				if (qs.get("section") === "sdcard") { return jsonResp({ sdcard: info.sdcard }); }
				return jsonResp(info);
			}
			if (p === "/topcards") {
				return jsonResp([
					{ id: "0009094506", fileOrUrl: "/Märchen/Hänsel und Gretel.mp3", count: 42 },
					{ id: "0006981923", fileOrUrl: "/Klassik", count: 31 },
					{ id: "0212345678", fileOrUrl: "/Hörspiele/Die Robo-Detektive", count: 27 }
				]);
			}
			if (p === "/savedSSIDs") { return jsonResp(["Heimnetz", "Gartenlaube"]); }
			if (p === "/activeSSID") { return jsonResp({ active: "Heimnetz" }); }
			if (p === "/eqrules") { return jsonResp([]); }
			if (p === "/homekit") { return jsonResp({ enabled: false, paired: false, code: "", deviceName: "ESPuino", tvName: "ESPuino TV" }); }
			// password status: the device never echoes the password back (write-only field)
			if (p === "/security") { return jsonResp({ enabled: false, cookieDays: 90 }); }
			if (p === "/bluetoothresults") { return jsonResp([]); }
			if (p === "/settings") {
				var section = qs.get("section");
				if (section === "defaults") { return jsonResp({ defaults: buildSettings() }); }
				return jsonResp(buildSettings());
			}
			// backup-upload status poll: pretend the (no-op) upload finished successfully
			if (p === "/backupupload") { return jsonResp({ status: 2, message: "demo: backup not actually uploaded" }); }
			// file-sync status poll: the demo can't really sync, so report it as done immediately
			if (p === "/sync") { return jsonResp({ status: 2, progress: 100, message: "demo: file sync runs only on the device" }); }
			// dry-run report: a small sample so the dry-run button shows something in the demo
			if (p === "/syncreport") {
				return { status: 200, contentType: "text/plain; charset=utf-8", body: [
					"# DRY RUN - nothing was changed. DL = would download, RM = would delete.",
					"DL new /Hoerspiele/Die Robo-Detektive/04.mp3",
					"DL chg /Maerchen/Haensel und Gretel.mp3",
					"RM   /Alte Folgen/Pumuckl 99.mp3",
					"RM   /Alte Folgen/Pumuckl 100.mp3",
					"# 2 to download, 2 to delete, 1305 total (demo sample)"
				].join("\n") + "\n" };
			}
			if (p === "/mode") { return jsonResp({ mode: 0 }); }
			if (p === "/activeequalizer") { return jsonResp({ gainLowPass: 0, gainBandPass: 0, gainHighPass: 0 }); }
			// now-playing detail for the info dialog (built from the now-playing fixture)
			if (p === "/currenttrack") {
				return jsonResp({
					active: true, title: TRACK.name, artist: TRACK.artist, album: TRACK.album,
					playMode: 3, isWebstream: false, rfidTag: "0212345678",
					elapsed: TRACK.time, duration: TRACK.duration,
					bitRate: 128000, sampleRate: 44100, channels: 2, codec: "MP3",
					trackNumber: TRACK.currentTrackNumber, totalTracks: TRACK.numberOfTracks,
					path: "/Hörspiele/Die Robo-Detektive/" + TRACK.name + ".mp3", size: 5012992
				});
			}
			// listening-stats ring buffer (used by the info dialog's 30-day chart)
			if (p === "/playstats") {
				var nowDay = Math.floor(Date.now() / 86400000);
				var days = [];
				for (var di = 0; di < 365; di++) { days.push(0); }
				// seed the last 30 days with a plausible daily listening time (seconds)
				var sample = [3600, 1800, 0, 5400, 2700, 900, 4500, 3000, 0, 6300];
				for (var k = 0; k < 30; k++) {
					days[(nowDay - k) % 365] = sample[k % sample.length];
				}
				return jsonResp({ lastDay: nowDay, days: days });
			}
		}

		// Everything that writes / triggers an action on the device is a no-op in the demo.
		if (/^\/(restart|shutdown|githubupdate|settings|sync|syncstop|rfidsync|backupupload|rfidnvserase|rfidresetpos|explorer|exploreraudio|homekit|security|bluetoothscan|bluetoothconnect|upload|savedSSIDs|trackcontrol|volume|ftp|webdav|logout)\b/.test(p)) {
			return jsonResp({ status: "ok", demo: true });
		}
		return null;
	}

	// ---------------------------------------------------------------
	// fetch() override
	// ---------------------------------------------------------------
	var realFetch = window.fetch ? window.fetch.bind(window) : null;
	window.fetch = function (input, init) {
		var url = (typeof input === "string") ? input : (input && input.url) || String(input);
		var method = (init && init.method) || (input && input.method) || "GET";
		var r = route(method, url);
		if (r && r.passthrough && realFetch) {
			var p = realFetch(r.passthrough, init);
			// merge the demo banner strings into the locales file
			if (r.lng) {
				return p.then(function (resp) {
					return resp.json().then(function (data) {
						data.demo = DEMO_I18N[r.lng] || DEMO_I18N.en;
						return new Response(JSON.stringify(data), { status: 200, headers: { "Content-Type": "application/json; charset=utf-8" } });
					});
				});
			}
			return p;
		}
		if (r) {
			return Promise.resolve(new Response(r.body, {
				status: r.status,
				headers: { "Content-Type": r.contentType }
			}));
		}
		return realFetch ? realFetch(input, init) : Promise.reject(new Error("no network in demo"));
	};

	// ---------------------------------------------------------------
	// XMLHttpRequest override (jQuery.ajax + uploads route through here)
	// ---------------------------------------------------------------
	var RealXHR = window.XMLHttpRequest;
	function MockXHR() {
		this._real = null;
		this._method = "GET";
		this._url = "";
		this._headers = {};
		this.readyState = 0;
		this.status = 0;
		this.statusText = "";
		this.responseText = "";
		this.response = "";
		this.responseType = "";
		this.responseURL = "";
		this.withCredentials = false;
		this.timeout = 0;
		this.upload = { addEventListener: function () {}, removeEventListener: function () {} };
		this.onreadystatechange = null;
		this.onload = null;
		this.onerror = null;
		this.onabort = null;
		this.ontimeout = null;
		this.onloadend = null;
		this._evt = {};
	}
	MockXHR.prototype.open = function (method, url) {
		this._method = method;
		this._url = url;
		this.readyState = 1;
		if (typeof this.onreadystatechange === "function") { this.onreadystatechange(); }
	};
	MockXHR.prototype.setRequestHeader = function (k, v) { this._headers[k] = v; };
	MockXHR.prototype.getResponseHeader = function (k) {
		if (/content-type/i.test(k)) { return "application/json; charset=utf-8"; }
		return null;
	};
	MockXHR.prototype.getAllResponseHeaders = function () { return "content-type: application/json; charset=utf-8\r\n"; };
	MockXHR.prototype.addEventListener = function (type, cb) { (this._evt[type] = this._evt[type] || []).push(cb); };
	MockXHR.prototype.removeEventListener = function (type, cb) {
		if (!this._evt[type]) { return; }
		this._evt[type] = this._evt[type].filter(function (f) { return f !== cb; });
	};
	MockXHR.prototype._fire = function (type) {
		var on = this["on" + type];
		if (typeof on === "function") { on.call(this, { type: type }); }
		(this._evt[type] || []).forEach(function (cb) { try { cb.call(this, { type: type }); } catch (e) {} }, this);
	};
	MockXHR.prototype.abort = function () { this._aborted = true; };
	MockXHR.prototype.send = function (body) {
		var self = this;
		var r = route(this._method, this._url);

		// Unknown or passthrough URL -> use a real XHR so static assets still load.
		if (!r || r.passthrough) {
			var real = new RealXHR();
			this._real = real;
			real.onreadystatechange = function () {
				self.readyState = real.readyState;
				self.status = real.status;
				self.statusText = real.statusText;
				try { self.responseText = real.responseText; } catch (e) {}
				try { self.response = real.response; } catch (e) {}
				// merge demo banner strings into the locales file (XHR path)
				if (real.readyState === 4 && r && r.lng && real.status >= 200 && real.status < 300) {
					try {
						var data = JSON.parse(real.responseText);
						data.demo = DEMO_I18N[r.lng] || DEMO_I18N.en;
						self.responseText = JSON.stringify(data);
						self.response = (self.responseType === "json") ? data : self.responseText;
					} catch (e) {}
				}
				if (typeof self.onreadystatechange === "function") { self.onreadystatechange(); }
				if (real.readyState === 4) { self._fire(real.status >= 200 && real.status < 300 ? "load" : "error"); self._fire("loadend"); }
			};
			real.open(this._method, (r && r.passthrough) ? r.passthrough : this._url, true);
			Object.keys(this._headers).forEach(function (k) { try { real.setRequestHeader(k, self._headers[k]); } catch (e) {} });
			if (this.responseType) { try { real.responseType = this.responseType; } catch (e) {} }
			real.send(body);
			return;
		}

		// Mocked endpoint -> answer asynchronously.
		setTimeout(function () {
			if (self._aborted) { return; }
			self.status = r.status;
			self.statusText = r.status === 200 ? "OK" : "Error";
			self.responseText = r.body;
			self.response = (self.responseType === "json") ? JSON.parse(r.body) : r.body;
			self.responseURL = self._url;
			self.readyState = 4;
			if (typeof self.onreadystatechange === "function") { self.onreadystatechange(); }
			self._fire(r.status >= 200 && r.status < 300 ? "load" : "error");
			self._fire("loadend");
		}, 60);
	};
	MockXHR.UNSENT = 0; MockXHR.OPENED = 1; MockXHR.HEADERS_RECEIVED = 2; MockXHR.LOADING = 3; MockXHR.DONE = 4;
	window.XMLHttpRequest = MockXHR;

	// ---------------------------------------------------------------
	// WebSocket override
	// ---------------------------------------------------------------
	function MockWebSocket(url) {
		var self = this;
		this.url = url;
		this.readyState = 0; // CONNECTING
		this.sendBuffer = [];
		this.onopen = null;
		this.onmessage = null;
		this.onclose = null;
		this.onerror = null;
		setTimeout(function () {
			self.readyState = 1; // OPEN
			if (typeof self.onopen === "function") { self.onopen({ type: "open" }); }
			// Proactively push the indicators the firmware would stream.
			self._emit({ opmode: 0 });
			self._emit({ volume: DEMO_VOLUME });
			self._emit({ rssi: -58 });
			self._emit({ battery: { level: 78, voltage: 3.94 } });
			self._emit({ ftpStatus: { running: false } });
			self._emit({ webdavStatus: { running: false } });
		}, 120);
	}
	MockWebSocket.prototype._emit = function (obj) {
		if (typeof this.onmessage === "function") { this.onmessage({ data: JSON.stringify(obj) }); }
	};
	MockWebSocket.prototype.send = function (data) {
		var msg;
		try { msg = JSON.parse(data); } catch (e) { return; }
		if ("settings" in msg) { this._emit({ settings: buildSettings() }); }
		if ("ssids" in msg) { this._emit({ settings: { ssids: { savedSSIDs: ["Heimnetz", "Gartenlaube"], active: "Heimnetz" } } }); }
		if ("trackinfo" in msg) { this._emit({ trackinfo: TRACK }); }
		if ("coverimg" in msg) { this._emit({ coverimg: true }); }
		if ("ping" in msg) { this._emit({ pong: "pong", controlsLocked: TRACK.controlsLocked, repeatMode: TRACK.repeatMode }); }
		if ("controls" in msg) { this._handleControl(msg.controls.action); }
	};
	// React to the player's control commands (same action codes as the firmware /
	// the mod-card list) and stream back the updated state so the UI reflects it.
	MockWebSocket.prototype._handleControl = function (action) {
		var changed = true;
		switch (Number(action)) {
			case 170: // play / pause
				TRACK.pausePlay = !TRACK.pausePlay;
				break;
			case 171: // previous track
			case 173: // first track
				TRACK.currentTrackNumber = (Number(action) === 173) ? 1 : Math.max(1, TRACK.currentTrackNumber - 1);
				TRACK.time = 0; TRACK.pausePlay = false; syncTrack();
				break;
			case 172: // next track
			case 174: // last track
				TRACK.currentTrackNumber = (Number(action) === 174) ? TRACK.numberOfTracks : Math.min(TRACK.numberOfTracks, TRACK.currentTrackNumber + 1);
				TRACK.time = 0; TRACK.pausePlay = false; syncTrack();
				break;
			case 190: // smart forward (in-file seek on a single long file, else next track)
			case 191: { // smart backward (in-file seek on a single long file, else previous track)
				var step = (Number(action) === 190) ? 300 : -300;
				TRACK.time = Math.max(0, Math.min(TRACK.duration, TRACK.time + step));
				TRACK.pausePlay = false; syncTrack();
				break;
			}
			case 175: // initial volume
				DEMO_VOLUME = 3; this._emit({ volume: DEMO_VOLUME }); changed = false;
				break;
			case 176: // louder
				DEMO_VOLUME = Math.min(21, DEMO_VOLUME + 1); this._emit({ volume: DEMO_VOLUME }); changed = false;
				break;
			case 177: // quieter
				DEMO_VOLUME = Math.max(0, DEMO_VOLUME - 1); this._emit({ volume: DEMO_VOLUME }); changed = false;
				break;
			case 100: // toggle key lock
				TRACK.controlsLocked = !TRACK.controlsLocked;
				break;
			case 120: // toggle night mode
				TRACK.nightMode = !TRACK.nightMode;
				break;
			case 105: TRACK.sleepTimerDuration = TRACK.sleepTimerDuration ? 0 : 1; break; // sleep after track
			case 106: TRACK.sleepTimerDuration = TRACK.sleepTimerDuration ? 0 : 1; break; // sleep after playlist
			case 111: TRACK.repeatMode = TRACK.repeatMode ? 0 : 1; break; // loop track
			default: changed = false; break;
		}
		if (changed) { syncTrack(); this._emit({ trackinfo: TRACK }); }
		this._emit({ status: "ok" });
	};
	MockWebSocket.prototype.close = function () {
		this.readyState = 3;
		if (typeof this.onclose === "function") { this.onclose({ type: "close", reason: "demo" }); }
	};
	MockWebSocket.prototype.addEventListener = function () {};
	MockWebSocket.CONNECTING = 0; MockWebSocket.OPEN = 1; MockWebSocket.CLOSING = 2; MockWebSocket.CLOSED = 3;
	window.WebSocket = MockWebSocket;

	console.log("%cESPuino demo mock active", "color:#0bf;font-weight:bold");
})();
