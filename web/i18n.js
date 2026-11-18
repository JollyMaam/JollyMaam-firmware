// Catalog keys identify presentation; firmware tokens remain the wire values.
let english = {}, translated = {}, fields = {};
export let language = "en";

export function useCatalog(source, translation = {}, mapping = {}) {
  english = source;
  translated = translation;
  fields = mapping;
}

export function text(key, parameters = {}, fallback = key) {
  const value = translated[key] ?? english[key] ?? fallback;
  return value.replace(/\{([a-zA-Z][a-zA-Z0-9_]*)\}/g,
    (match, name) => Object.hasOwn(parameters, name) ? String(parameters[name]) : match);
}

export function fieldText(field, part, fallback) {
  const prefix = fields[`${field.kind}:${field.id}`];
  if (!prefix) return fallback;
  // The rule count differs between boards; never copy the exporter's limit.
  return text(`${prefix}.${part}`, { count: field.size / 10 }, fallback);
}

export async function loadCatalog(locale, fetcher = fetch) {
  const read = async (name) => {
    const response = await fetcher(new URL(`./strings/${name}.json`, import.meta.url));
    if (!response.ok) throw new Error(`Cannot load string catalog: ${name}`);
    return response.json();
  };
  const [source, mapping] = await Promise.all([read("en"), read("settings")]);
  const requested = String(locale || "en").toLowerCase();
  let translation = {};
  language = "en";
  // Locale input is a filename, never a path. Regional locales fall back to
  // their base language, then English. Catalogs are bundled, never remote.
  if (/^[a-z]{2,3}(-[a-z0-9]{2,8})*$/.test(requested)) {
    for (const name of [...new Set([requested, requested.split("-")[0]])]) {
      if (name === "en") break;
      try { translation = await read(name); language = name; break; } catch { /* English fallback */ }
    }
  }
  useCatalog(source, translation, mapping);
}
