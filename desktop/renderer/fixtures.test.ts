import { describe, expect, it } from 'vitest';

import { mockConnection, mockDiagnostics, mockProjects, mockTasks } from './fixtures.js';
import { isActiveTask, projectLabel } from './view-model.js';

describe('mock fixtures', () => {
  it('ships an unconfigured connection', () => {
    expect(mockConnection.configured).toBe(false);
    expect(mockConnection.connected).toBe(false);
    expect(mockConnection.tokenHint).toBeNull();
  });

  it('ships projects that resolve by id', () => {
    expect(mockProjects.length).toBeGreaterThan(0);
    for (const project of mockProjects) {
      expect(projectLabel(mockProjects, project.projectId)).toBe(project.displayName);
    }
  });

  it('ships at least one active and one terminal task', () => {
    expect(mockTasks.some((task) => isActiveTask(task.state))).toBe(true);
    expect(mockTasks.some((task) => !isActiveTask(task.state))).toBe(true);
  });

  it('ships diagnostics without a data directory', () => {
    expect(mockDiagnostics.databasePath).toBeNull();
    expect(mockDiagnostics.protocolVersions).toContain('1.0');
  });
});
