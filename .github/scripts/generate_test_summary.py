#!/usr/bin/env python3
"""
Generate a markdown summary of pytest results from JSON report.
Outputs formatted markdown suitable for GitHub Step Summary.
Uses pytest-json-report format.
"""

import json
import sys
from pathlib import Path
from typing import Any, Dict, List

def parse_report(report_path: str) -> Dict[str, Any]:
    """Parse pytest JSON report."""
    with open(report_path, 'r') as f:
        return json.load(f)

def categorize_tests(report: Dict[str, Any]) -> Dict[str, List[Dict]]:
    """Categorize tests by outcome."""
    categories = {
        'passed': [],
        'failed': [],
        'error': [],
        'skipped': []
    }
    
    for test in report.get('tests', []):
        outcome = test.get('outcome', 'unknown')
        if outcome in categories:
            categories[outcome].append(test)
    
    return categories

def format_summary(report: Dict[str, Any], categories: Dict[str, List[Dict]]) -> str:
    """Generate markdown summary of test results."""
    summary = report.get('summary', {})
    
    total = summary.get('total', 0)
    passed = len(categories['passed'])
    failed = len(categories['failed'])
    errors = len(categories['error'])
    skipped = len(categories['skipped'])
    
    duration = summary.get('duration', 0)
    
    # Determine status emoji
    if errors > 0 or failed > 0:
        status_emoji = "❌"
        status = "FAILED"
    elif passed > 0:
        status_emoji = "✅"
        status = "PASSED"
    else:
        status_emoji = "⏭️"
        status = "SKIPPED"
    
    lines = [
        f"**Status:** {status} {status_emoji} | **Duration:** {duration:.2f}s",
        "",
        "| Outcome | Count |",
        "|---------|-------|",
        f"| ✅ Passed | {passed} |",
    ]
    
    if failed > 0:
        lines.append(f"| ❌ Failed | {failed} |")
    if errors > 0:
        lines.append(f"| 🔥 Errors | {errors} |")
    if skipped > 0:
        lines.append(f"| ⏭️ Skipped | {skipped} |")
    
    lines.append(f"| **Total** | **{total}** |")
    lines.append("")
    
    # Add details for failed/error tests
    if failed > 0 or errors > 0:
        lines.append("#### Failed/Error Tests")
        lines.append("")
        
        for test in categories['failed'] + categories['error']:
            test_name = test.get('nodeid', 'unknown').split('::')[-1]
            outcome = test.get('outcome', 'unknown').upper()
            lines.append(f"- **{outcome}:** `{test_name}`")
        
        lines.append("")
    
    # Add passed tests summary (collapsed if many)
    if categories['passed']:
        if len(categories['passed']) > 5:
            lines.append(f"<details><summary><b>✅ Passed Tests ({passed})</b></summary>")
            lines.append("")
        else:
            lines.append("#### Passed Tests")
            lines.append("")
        
        for test in categories['passed']:
            test_name = test.get('nodeid', 'unknown').split('::')[-1]
            lines.append(f"- `{test_name}`")
        
        if len(categories['passed']) > 5:
            lines.append("</details>")
        
        lines.append("")
    
    return "\n".join(lines)

def main():
    if len(sys.argv) < 2:
        print("Usage: generate_test_summary.py <report.json>", file=sys.stderr)
        sys.exit(1)
    
    report_path = sys.argv[1]
    
    if not Path(report_path).exists():
        print(f"Report file not found: {report_path}", file=sys.stderr)
        sys.exit(1)
    
    try:
        report = parse_report(report_path)
        categories = categorize_tests(report)
        summary = format_summary(report, categories)
        print(summary)
    except Exception as e:
        print(f"Error processing report: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()

