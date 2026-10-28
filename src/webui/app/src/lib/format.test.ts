import { describe, expect, it } from 'vitest';
import { formatBytes, formatCount, niceTimeOffset, shortDuration, shortHash } from './format';
import { syncStatus, type SyncInput } from './sync';

describe('format', () => {
  it('says how long ago as chains-qt', () => {
    expect(niceTimeOffset(1)).toBe('1 second');
    expect(niceTimeOffset(59)).toBe('59 seconds');
    expect(niceTimeOffset(60 * 90)).toBe('90 minutes');
    expect(niceTimeOffset(3600 * 30)).toBe('30 hours');
    expect(niceTimeOffset(86400 * 3)).toBe('3 days');
    expect(niceTimeOffset(86400 * 30)).toBe('4 weeks');
    expect(niceTimeOffset(31556952 + 86400 * 14)).toBe('1 year and 2 weeks');
  });
  it('sizes, counts, hashes', () => {
    expect(formatBytes(999)).toBe('999 B');
    expect(formatBytes(1500)).toBe('1 kB');
    expect(formatBytes(2_500_000)).toBe('2 MB');
    expect(formatCount(1234567)).toBe('1 234 567');
    expect(shortDuration(90061)).toBe('1 d 1 h');
    expect(shortDuration(0)).toBe('0 s');
    expect(shortHash('0123456789abcdef0123456789abcdef', 4)).toBe('0123…cdef');
  });
});

describe('sync status', () => {
  const base: SyncInput = {
    tipHeight: 100,
    tipTime: 1000,
    progress: 0.5,
    loadingBlocks: false,
    headerHeight: 100,
    headerTime: 1000,
    connections: 3,
    networkActive: true,
    spacing: 60,
    now: 1000,
  };
  it('is up to date with a recent tip', () => {
    expect(syncStatus(base)).toMatchObject({ upToDate: true, progress: 1 });
  });
  it('says what it does when behind', () => {
    const now = 1000 + 5 * 3600;
    expect(syncStatus({ ...base, now, headerTime: now })).toMatchObject({ upToDate: false, label: 'Synchronizing with network…', behind: '5 hours behind' });
    expect(syncStatus({ ...base, now })).toMatchObject({ label: 'Syncing Headers (25.0%)…' });
    expect(syncStatus({ ...base, now, presyncHeight: 200, presyncTime: 1000 + 200 * 60 })).toMatchObject({ label: expect.stringMatching(/^Pre-syncing Headers/) });
    expect(syncStatus({ ...base, now, loadingBlocks: true })).toMatchObject({ label: 'Processing blocks on disk…' });
    expect(syncStatus({ ...base, now, connections: 0 })).toMatchObject({ label: 'Waiting for peers…' });
    expect(syncStatus({ ...base, now, networkActive: false })).toMatchObject({ label: 'Network activity disabled' });
  });
  it('is never up to date at block 0, without peers, in initial download or with headers ahead of the blocks', () => {
    expect(syncStatus({ ...base, tipHeight: 0, headerHeight: 0 })).toMatchObject({ upToDate: false, label: 'Synchronizing with network…', behind: '' });
    expect(syncStatus({ ...base, connections: 0 })).toMatchObject({ upToDate: false, label: 'Waiting for peers…' });
    expect(syncStatus({ ...base, connections: 0, solo: true })).toMatchObject({ upToDate: true });
    expect(syncStatus({ ...base, ibd: true })).toMatchObject({ upToDate: false });
    expect(syncStatus({ ...base, headerHeight: 120 })).toMatchObject({ upToDate: false, label: 'Syncing (100 / 120)…' });
  });
  it('shows a drop-in node\'s own judgement, whatever the time of its tip', () => {
    // Synced with a last block hours old (a chain that makes a block only when one is merge-mined).
    expect(syncStatus({ ...base, now: 1000 + 5 * 3600, sync: 'synced', target: 100 })).toMatchObject({ upToDate: true });
    expect(syncStatus({ ...base, sync: 'syncing', target: 2343, progress: 0.04 })).toMatchObject({ upToDate: false, label: `Syncing (100 / ${formatCount(2343)})…`, progress: 0.04 });
    expect(syncStatus({ ...base, sync: 'syncing', target: -1 })).toMatchObject({ label: 'Syncing (100 / ?)…' });
    expect(syncStatus({ ...base, tipHeight: 0, tipTime: 0, sync: 'waiting', target: -1 })).toMatchObject({ upToDate: false, label: 'Waiting for peers…', behind: '' });
  });
});
