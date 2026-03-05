# Rescue Guide

This guide provides step-by-step instructions for responding to system failures, data corruption, or other critical incidents.

## Overview

The purpose of this guide is to ensure rapid and consistent recovery from unexpected disruptions. It covers:
- Emergency contact information
- Recovery procedures for common failure modes
- Verification steps post-recovery
- Post-mortem documentation requirements

## Emergency Contacts

| Role | Name | Phone | Email |
|------|------|-------|-------|
| On-Call Engineer | See PagerDuty | +1-555-ONCALL | oncall@example.com |
| Security Lead | Jane Doe | +1-555-SECURE | security@example.com |
| Infrastructure Lead | John Smith | +1-555-INFRA | infra@example.com |

> **Note**: Always verify contact details via internal directory before use.

## Common Failure Scenarios & Recovery Steps

### 1. Database Corruption

**Symptoms**:
- Queries failing with `SQLSTATE[HY000] [2006] MySQL server has gone away`
- Replication lag > 30 seconds consistently
- Checksum mismatches in logs

**Recovery Steps**:
1. Identify the corrupted table: `CHECK TABLE <table_name> EXTENDED;`
2. Stop writes to affected database (if possible)
3. Restore from latest known-good backup using:
   ```bash
   mysql -u root -p < /backups/db-full-$(date -d 'yesterday' +%Y%m%d).sql.gz
   ```
4. Run `REPAIR TABLE <table_name>;`
5. Validate integrity with checksum comparison tool (`pt-table-checksum`)
6. Resume application traffic gradually

### 2. Service Outage (HTTP 5xx Spike)

**Symptoms**:
- Spike in error rate (>5% over baseline)
- Increased latency (>2s p95)
- High memory/CPU utilization on app servers

**Recovery Steps**:
1. Check logs: `kubectl logs -l app=myapp --tail=100 | grep ERROR`
2. Roll back recent deployment if timing aligns:
   ```bash
   kubectl rollout undo deployment/myapp
   ```
3. Scale up replicas temporarily if resource exhaustion suspected:
   ```bash
   kubectl scale deployment myapp --replicas=5
   ```
4. Enable circuit breakers in service mesh (e.g., Istio)
5. Monitor metrics dashboard for stabilization

## Verification Checklist

After recovery, confirm:
- [ ] All services report healthy status (`/health` endpoints return 200)
- [ ] Metrics match pre-failure baselines (within 10% tolerance)
- [ ] End-to-end user flows tested in staging or canary environment
- [ ] Monitoring alerts are functioning correctly

## Post-Mortem Requirements

Within **48 hours** of resolution:

1. Complete incident timeline with timestamps (UTC)
2. Identify root cause and contributing factors
3. List actions taken, including decisions that delayed recovery
4. Document lessons learned and proposed improvements
5. Share draft internally for review before finalizing
6. Archive in `docs/incidents/YYYY-MM-DD-title.md`

> Use [this template](https://example.com/postmortem-template) as a starting point.

## Appendix: Tools & Resources

- Incident Command System (ICS): `/ops/ics`
- Runbooks Index: `/runbooks`
- PagerDuty Escalation Policy: `#production-alerts`
- Slack Channel: `#incidents`
