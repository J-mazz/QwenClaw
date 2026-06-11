/**
 * Stale-response guard for async loaders.
 *
 * Each call to `beginLoad(state, key)` invalidates any earlier load with the
 * same key and returns an `isCurrent()` check. Loaders call it before issuing
 * a request and skip applying results when a newer load has started since:
 *
 *   const isCurrent = beginLoad(state, "cronJobs");
 *   const res = await client.request(...);
 *   if (!isCurrent()) return;
 */
type SeqHost = { __loadSeqs?: Record<string, number> };

export function beginLoad(state: object, key: string): () => boolean {
  const host = state as SeqHost;
  const seqs = (host.__loadSeqs ??= {});
  const seq = (seqs[key] ?? 0) + 1;
  seqs[key] = seq;
  return () => seqs[key] === seq;
}
