// Browser check: flashing and the USB console use Web Serial: Chromium-based desktop
// browsers, and Firefox 151 or newer on desktop. Shown at the top of every page when it's missing.
(() => {
  if ("serial" in navigator) return;
  const ua = navigator.userAgent;
  const mobile = /Android|iPhone|iPad|Mobile/i.test(ua);
  const ff = /Firefox\/(\d+)/.exec(ua);
  const name = ff ? "Firefox" : /Edg\//.test(ua) ? "Edge" : /OPR\//.test(ua) ? "Opera"
    : /Chrome\//.test(ua) ? "this Chromium browser" : /Safari\//.test(ua) ? "Safari" : "this browser";
  const why = mobile
    ? `Phones and tablets can't flash boards or connect over USB.`
    : ff ? `Firefox ${ff[1]} can't talk to USB devices. <b>Firefox 151 and newer can</b>: update Firefox (menu, Help, About Firefox) and reload.`
    : `${name} can't talk to USB devices (it has no Web Serial), so installing firmware and the USB console won't work here.`;
  const fix = mobile
    ? `On a phone, join the master's own Wi-Fi (<b>SkySentinel-XXXX</b>) instead: the console opens in any browser.`
    : `Or use <b>Chrome</b> or <b>Edge</b> on a desktop (Opera and Vivaldi usually work too; Brave may need Web Serial turned on in its settings).
       The console also works in <b>any</b> browser over the master's own Wi-Fi: join <b>SkySentinel-XXXX</b> and it opens by itself.`;
  const bar = document.createElement("div");
  bar.setAttribute("role", "alert");
  bar.style.cssText = "background:#2A1E0B;border-bottom:1px solid #F2A93B;color:#F2C27A;padding:10px 20px;font-size:15px;line-height:1.45";
  bar.innerHTML = `<div style="max-width:1120px;margin:0 auto"><b style="color:#F2A93B;letter-spacing:1px">WRONG BROWSER FOR USB.</b> ${why} ${fix}</div>`;
  const insert = () => document.body.insertBefore(bar, document.body.firstChild);
  if (document.body) insert(); else document.addEventListener("DOMContentLoaded", insert);
})();
