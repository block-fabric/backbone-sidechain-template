// What the status bar says about the sync of the node (BitcoinGUI::setNumBlocks).
import { formatCount, niceTimeOffset } from './format';
import { t, tn } from './i18n.svelte';

/** A tip older than this is "behind" (MAX_BLOCK_TIME_GAP of chains-qt). */
export const MAX_BLOCK_TIME_GAP = 90 * 60;
/** Headers further behind than this many blocks are being synced (HEADER_HEIGHT_DELTA_SYNC). */
export const HEADER_HEIGHT_DELTA_SYNC = 24;

export interface SyncInput {
  tipHeight: number;
  tipTime: number;
  progress: number;
  loadingBlocks: boolean;
  headerHeight: number;
  headerTime: number;
  presyncHeight?: number;
  presyncTime?: number;
  connections: number;
  networkActive: boolean;
  /** Seconds between blocks on this network. */
  spacing: number;
  /** Now, in seconds. */
  now: number;
  /** In initial block download (the node's tip event): never up to date. */
  ibd?: boolean;
  /** A drop-in node's own judgement (its tip event's sync, bt::appui::sync_state), shown as it is. */
  sync?: 'synced' | 'syncing' | 'waiting';
  /** With sync: the best height known (the peers' best), -1 or undefined when not known yet. */
  target?: number;
  /** A network where a node alone is normal (regtest): up to date without peers. */
  solo?: boolean;
}

export interface SyncStatus {
  upToDate: boolean;
  /** What the node does: "Synchronizing with network…". Empty when up to date. */
  label: string;
  /** "3 hours behind", when behind (in the language shown, as label and detail). */
  behind: string;
  /** Verification progress, 0 to 1. */
  progress: number;
  /** Longer text for a tooltip. */
  detail: string;
}

function percent(height: number, left: number): string {
  return ((100 * height) / (height + left)).toFixed(1);
}

export function syncStatus(s: SyncInput): SyncStatus {
  const secs = s.now - s.tipTime;
  // In the language shown: every message here is one of the app, with its own placeholders.
  const processed = tn('Processed %n block(s) of transaction history.', s.tipHeight);
  const upToDate: SyncStatus = { upToDate: true, label: '', behind: '', progress: 1, detail: `${t('Up to date')}. ${processed}` };
  const recent = secs < MAX_BLOCK_TIME_GAP;
  const behindText = s.tipTime > 0 && !recent ? t('%1 behind', niceTimeOffset(secs)) : '';
  const catchingUp = (label: string, progress: number): SyncStatus => ({
    upToDate: false,
    label,
    behind: behindText,
    progress: Math.max(0, Math.min(1, progress)),
    detail: [t('Catching up…'), processed, ...(behindText ? [t('Last received block was generated %1 ago.', niceTimeOffset(secs)), t('Transactions after this will not yet be visible.')] : [])].join(' '),
  });
  const syncing = (target: number) => t('Syncing (%1 / %2)…', formatCount(s.tipHeight), target >= 0 ? formatCount(target) : '?');

  // A drop-in node judges itself (peers, height, the peers' best, the mainchain read): a chain that makes a block only
  // when someone merge-mines one may have a last block hours old and be fully synced, or a fresh one and be behind.
  if (s.sync) {
    if (s.sync === 'synced') return upToDate;
    if (s.sync === 'waiting') return catchingUp(t('Waiting for peers…'), s.progress);
    return catchingUp(syncing(s.target ?? -1), s.progress);
  }

  // A Core node: a recent tip (BitcoinGUI::setNumBlocks), and never at block 0, in initial block download, without
  // peers (but on a network where a node alone is normal), or with headers it has not the blocks of.
  const peers = s.connections > 0 || !!s.solo;
  if (recent && s.tipHeight > 0 && !s.ibd && peers && s.networkActive && s.headerHeight <= s.tipHeight) return upToDate;
  let label: string;
  if (s.loadingBlocks) {
    // While a reindex reads the block files, the headers run ahead of the blocks (BlockSource::DISK, a
    // header tip): "Indexing"; then the blocks are connected: "Processing".
    label = s.headerHeight > s.tipHeight ? t('Indexing blocks on disk…') : t('Processing blocks on disk…');
  } else if (!s.networkActive) {
    label = t('Network activity disabled');
  } else if (!peers) {
    label = t('Waiting for peers…');
  } else {
    label = t('Synchronizing with network…');
    const headersLeft = Math.floor((s.now - s.headerTime) / s.spacing);
    if (s.presyncHeight !== undefined && s.presyncTime !== undefined && s.presyncHeight > s.headerHeight) {
      const left = Math.floor((s.now - s.presyncTime) / s.spacing);
      if (left > HEADER_HEIGHT_DELTA_SYNC) label = t('Pre-syncing Headers (%1%)…', percent(s.presyncHeight, left));
    } else if (headersLeft > HEADER_HEIGHT_DELTA_SYNC) {
      label = t('Syncing Headers (%1%)…', percent(s.headerHeight, headersLeft));
    } else if (s.headerHeight > s.tipHeight) {
      label = syncing(s.headerHeight);
    }
  }
  return catchingUp(label, s.progress);
}
