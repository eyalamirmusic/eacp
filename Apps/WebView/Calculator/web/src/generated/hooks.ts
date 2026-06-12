// Re-exports the codegen `<basename>.hooks` module under the stable
// name `hooks`, so user imports stay as `./generated/hooks` regardless
// of the underlying schema basename. The Miro hooks emitter already
// imports from `./backend` and `./react` (eacp conventions), so this
// shim is a pure re-export with no transformation.
export * from './schema.hooks';
