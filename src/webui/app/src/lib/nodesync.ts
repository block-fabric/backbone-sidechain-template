// The sync status of the node shown by the status bar and the homes (lib/sync.ts), from the node's state: one place, so
// that every indicator says the same.
import { app, node } from './state.svelte';
import { syncStatus, type SyncStatus } from './sync';

/** The node's sync status at `now` (seconds), or null before its first tip. */
export function nodeSyncStatus(now: number): SyncStatus | null {
  const tip = node.tip;
  if (!tip) return null;
  return syncStatus({
    tipHeight: tip.height,
    tipTime: tip.time,
    progress: tip.progress,
    loadingBlocks: tip.loadingBlocks,
    headerHeight: node.header?.height ?? tip.height,
    headerTime: node.header?.time ?? tip.time,
    presyncHeight: node.presync?.height,
    presyncTime: node.presync?.time,
    connections: node.connections,
    networkActive: node.networkActive,
    spacing: app.info?.blockSpacing ?? 60,
    now,
    ibd: tip.ibd,
    sync: tip.sync,
    target: tip.target,
    solo: app.info?.network === 'regtest',
  });
}
