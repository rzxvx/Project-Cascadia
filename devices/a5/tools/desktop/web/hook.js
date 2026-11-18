// cascadia-web's page script (tools/desktop/web): run in every frame of every
// page, before the page's own scripts.
//
// A video started here is stopped at once, and what is known of it goes to the
// browser, which plays it in mpv (cascadia-play): WebKit plays video through
// GStreamer and the page's compositing, which this iPad does not do at the
// video's speed; mpv does.  What goes:
//   frame      this frame's address: an embedded player's page is the video's
//              page, the address yt-dlp knows best
//   src        the video's own address, when it has one (not the blob: of a
//              Media Source player)
//   videos     how many videos this frame has: with several, the page alone
//              does not say which one was tapped
//   manifests  the HLS and DASH playlists this frame fetched
//   tapped     whether a tap started it (transient user activation)
// A muted video that starts by itself (a preview, a moving background) is
// stopped and not passed on, and so is one too small to be what was tapped.
// The browser also checks that a tap came: a page cannot start the player.
// A video that cannot start here at all goes on when a tap asks it to (below).
//
// {cascadia: 'bypass'}, posted to the top frame by the browser ("Play here"
// after the player failed): every frame lets its videos play in the page for
// a minute, and the one handed over starts again.
//
// Also: a text field that takes the focus after a tap asks for the on-screen
// keyboard, and lets it go when it loses the focus.
(() => {
    'use strict';
    const H = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.cascadia;
    if (!H || window.__cascadiaHook)
        return;
    Object.defineProperty(window, '__cascadiaHook', {value: true});
    const post = (m) => { try { H.postMessage(JSON.stringify(m)); } catch (e) { } };
    const tapped = () => navigator.userActivation ? navigator.userActivation.isActive : true;
    const real = (u) => /^https?:/i.test(u || '');
    const MANIFEST = /\.(m3u8|mpd)(\?|#|$)|[?&](format|type)=(m3u8|mpd|hls|dash)\b/i;
    let bypassUntil = 0, handed = null;

    function srcOf(v) {
        if (real(v.currentSrc))
            return v.currentSrc;
        if (real(v.src))
            return v.src;
        for (const s of v.querySelectorAll('source[src]'))
            if (real(s.src))
                return s.src;
        return '';
    }

    function manifests() {
        const out = [];
        try {
            for (const e of performance.getEntriesByType('resource'))
                if (MANIFEST.test(e.name) && !out.includes(e.name))
                    out.push(e.name);
        } catch (e) { }
        return out.slice(0, 8);
    }

    function handOver(v, t) {
        const r = v.getBoundingClientRect();
        if ((v.muted && !t) || r.width < 100 || r.height < 56 || document.visibilityState === 'hidden')
            return;
        handed = v;
        v.__cascadiaWanted = 0;
        post({kind: 'play', frame: location.href, top: window === window.top, src: srcOf(v),
              tapped: t, videos: document.getElementsByTagName('video').length,
              manifests: manifests(), title: document.title || ''});
    }

    // A tap on what looks like a player counts for 90 s in this frame: a
    // site that loads its player when the poster is tapped may call play()
    // well after the tap's activation is gone (Wikimedia Commons: 52 s here)
    let playerTapped = 0;
    const PLAYERISH = 'video, audio, [class*="player" i], [class*="video" i], [id*="player" i], ' +
                      '[aria-label*="play" i], [title*="play" i]';
    document.addEventListener('pointerdown', (e) => {
        try {
            if (e.target.closest && e.target.closest(PLAYERISH))
                playerTapped = Date.now();
        } catch (x) { }
    }, {capture: true, passive: true});

    function onPlay(e) {
        const v = e.target;
        if (!(v instanceof HTMLVideoElement) || Date.now() < bypassUntil)
            return;
        try { v.pause(); } catch (x) { }
        handOver(v, tapped() || Date.now() - playerTapped < 90000);
    }

    // A video WebKit cannot play (HLS by its address, which WebKitGTK does
    // not take and some sites give it anyway; a codec it lacks) never starts, so
    // it never sends 'play': the play() a tap makes on it hands it on, and so
    // does the error that ends a load a tap asked for.
    const play = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function () {
        if (this instanceof HTMLVideoElement && Date.now() >= bypassUntil && tapped()) {
            this.__cascadiaWanted = Date.now();
            if (this.error || this.networkState === HTMLMediaElement.NETWORK_NO_SOURCE)
                handOver(this, true);
        }
        return play.apply(this, arguments);
    };

    function onError(e) {
        let v = e.target;
        if (v instanceof HTMLSourceElement)
            v = v.parentElement;
        if (v instanceof HTMLVideoElement && v.__cascadiaWanted && Date.now() - v.__cascadiaWanted < 20000)
            handOver(v, true);
    }

    // A video from a Blob's address (URL.createObjectURL of a Blob or File;
    // not of a MediaSource, which is another player) is not given to WebKit:
    // WebKitGTK 2.48 with GStreamer 1.28 crashes the page process as its
    // pipeline makes the source for it (2026-11-17, in GObject under
    // urisourcebin) -- archive.org's mobile page does it to test autoplay.
    // Refused, the video has no source, as with a format WebKit does not
    // play; and the player could not have opened a Blob, held in the page.
    const blobs = new Set();
    const createURL = URL.createObjectURL, revokeURL = URL.revokeObjectURL;
    URL.createObjectURL = function (o) {
        const u = createURL.apply(this, arguments);
        if (o instanceof Blob)
            blobs.add(u);
        return u;
    };
    URL.revokeObjectURL = function (u) {
        blobs.delete(String(u));
        return revokeURL.apply(this, arguments);
    };
    const guard = (proto) => {
        const d = Object.getOwnPropertyDescriptor(proto, 'src');
        if (d && d.set)
            Object.defineProperty(proto, 'src', {
                configurable: true, enumerable: d.enumerable, get: d.get,
                set(v) { if (!blobs.has(String(v))) d.set.call(this, v); }
            });
    };
    guard(HTMLMediaElement.prototype);
    guard(HTMLSourceElement.prototype);
    const setAttribute = Element.prototype.setAttribute;
    Element.prototype.setAttribute = function (name, value) {
        if ((this instanceof HTMLMediaElement || this instanceof HTMLSourceElement) &&
            String(name).toLowerCase() === 'src' && blobs.has(String(value)))
            return;
        return setAttribute.apply(this, arguments);
    };

    // media events neither bubble nor leave a shadow root: listen in each one
    const hook = (root) => {
        root.addEventListener('play', onPlay, true);
        root.addEventListener('error', onError, true);
    };
    hook(document);
    const attach = Element.prototype.attachShadow;
    if (attach)
        Element.prototype.attachShadow = function (init) {
            const root = attach.call(this, init);
            try { hook(root); } catch (x) { }
            return root;
        };

    window.addEventListener('message', (e) => {
        if (!e.data || e.data.cascadia !== 'bypass')
            return;
        bypassUntil = Date.now() + 60000;
        for (let i = 0; i < window.frames.length; i++)
            try { window.frames[i].postMessage(e.data, '*'); } catch (x) { }
        if (handed)
            try { handed.play(); } catch (x) { }
    });

    const TEXT = /^(text|search|url|email|tel|password|number)$/i;
    const editable = (el) => !!el && (el.isContentEditable || el.tagName === 'TEXTAREA' ||
                                      (el.tagName === 'INPUT' && TEXT.test(el.type || 'text')));
    document.addEventListener('focusin', (e) => {
        if (editable(e.target) && tapped())
            post({kind: 'kbd', show: true});
    }, true);
    document.addEventListener('focusout', (e) => {
        if (editable(e.target))
            post({kind: 'kbd', show: false});
    }, true);
})();
