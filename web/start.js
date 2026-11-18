import { loadCatalog, language, text } from "./i18n.js";
const locale = new URLSearchParams(location.search).get("lang") || navigator.language;
try {
  await loadCatalog(locale);
  document.documentElement.lang = language;
  for (const element of document.querySelectorAll("[data-i18n]")) {
    element.textContent = text(element.dataset.i18n);
  }
  await import("./app.js");
} catch (error) {
  const message = document.createElement("p");
  message.setAttribute("role", "alert");
  message.textContent = String(error.message || error);
  document.querySelector("main").prepend(message);
  console.error(error);
}
