import {flashViaRadio} from "./uart-flasher.mjs";
const start = document.querySelector("#uart-update");
const cancel = document.querySelector("#uart-cancel");
const message = document.querySelector("#uart-status");
const progress = document.querySelector("#uart-progress");
let controller;
start.addEventListener("click", async () => {
  if (!navigator.serial) {message.textContent="Use desktop Chrome or Edge with Web Serial.";return;}
  const board = document.querySelector("#board");
  const profile = document.querySelector("#uart-profile");
  const family = {"c5":"ESP32-C5", "c3-supermini":"ESP32-C3"}[board.value];
  if (!family || board.disabled) return;
  controller = new AbortController();
  start.disabled = true; cancel.disabled = false; board.disabled = true;
  profile.disabled = true; progress.value = 0;
  document.querySelector("#ble-update").disabled = true;
  try {
    await flashViaRadio({
      board:board.value, profile:profile.value, family, signal:controller.signal,
      status:text => {message.textContent=text;},
      progress:value => {progress.value=value;}
    });
  } catch { /* flashViaRadio reports the precise unconfirmed state */ }
  finally {
    controller = undefined; start.disabled = false; cancel.disabled = true;
    board.disabled = false; profile.disabled = false;
    document.querySelector("#ble-update").disabled = false;
  }
});
cancel.addEventListener("click", () => controller?.abort());
