# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Build a coverage ledger from the public DDI0487A.k PDF's instruction index.

Requires pymupdf. The PDF is intentionally not redistributed in this repository.
Mnemonic-level evidence is NOT proof of every form/operand/encoding in a section.
Usage: python tests/a64_audit_catalogue.py manual.pdf audit.json output.tsv
"""

import csv
import json
import re
import sys
from pathlib import Path

import pymupdf

PROXIES = {
    'AT',
    'DC',
    'IC',
    'TLBI',
    'SYS',
    'MRS',
    'MSR',
    'SVC',
    'BRK',
    'HINT',
    'NOP',
    'YIELD',
    'WFE',
    'WFI',
    'SEV',
    'SEVL',
    'DMB',
    'DSB',
    'ISB',
    'CLREX',
}
UNSUPPORTED = {'ERET', 'DRPS', 'HVC', 'SMC', 'HLT', 'DCPS1', 'DCPS2', 'DCPS3', 'SYSL'}
ALIASES = {
    'ASRV': {'ASR'},
    'LSLV': {'LSL'},
    'LSRV': {'LSR'},
    'RORV': {'ROR'},
    'BFM': {'BFI', 'BFXIL'},
    'SBFM': {'SBFIZ', 'SBFX', 'SXTB', 'SXTH', 'SXTW', 'ASR'},
    'UBFM': {'UBFIZ', 'UBFX', 'UXTB', 'UXTH', 'LSL', 'LSR'},
    'MOVZ': {'MOV'},
    'MOVN': {'MOV'},
    'MADD': {'MUL'},
    'SMADDL': {'SMULL'},
    'UMADDL': {'UMULL'},
    'CSET': {'CSINC'},
    'CSETM': {'CSINV'},
    'CINC': {'CSINC'},
    'CINV': {'CSINV'},
    'CNEG': {'CSNEG'},
    'CMP': {'SUBS'},
    'CMN': {'ADDS'},
    'NEG': {'SUB'},
    'NEGS': {'SUBS'},
    'MVN': {'ORN'},
    'TST': {'ANDS'},
    'NGC': {'SBC'},
    'NGCS': {'SBCS'},
    'SXTW': {'SBFX', 'SBFM'},
    'SXTH': {'SBFX', 'SBFM'},
    'SXTB': {'SBFX', 'SBFM'},
    'UXTB': {'UBFX', 'UBFM'},
    'UXTH': {'UBFX', 'UBFM'},
    'SBFIZ': {'SBFM'},
    'SBFX': {'SBFM'},
    'UBFIZ': {'UBFM'},
    'UBFX': {'UBFM'},
    'BFI': {'BFM'},
    'BFXIL': {'BFM'},
    'SMNEGL': {'SMSUBL'},
    'UMNEGL': {'UMSUBL'},
    'MNEG': {'MSUB'},
    'UXTL': {'USHLL', 'USHLL2'},
    'SXTL': {'SSHLL', 'SSHLL2'},
    'PRFUM': {'PRFM'},
    'INS': {'MOV'},
    'NOT': {'MVN'},
}


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    pdf, audit, output = map(Path, sys.argv[1:])
    doc = pymupdf.open(pdf)
    if 'DDI 0487A.k' not in doc[0].get_text():
        raise SystemExit('Expected DDI0487A.k, not a different instruction index')
    cases = json.loads(audit.read_text())['cases']
    rows = []
    for level, title, page in doc.get_toc():
        if level != 3 or not re.match(r'C[67]\.2\.\d+ ', title):
            continue
        section, description = title.split(' ', 1)
        names = set(description.split(' (')[0].replace(',', '').split())
        matched = set(names)
        for name in names:
            matched |= ALIASES.get(name, set())
        if 'B.cond' in names:
            matched |= {e['instruction'].split()[0].upper() for e in cases if e['instruction'].startswith('b.')}
        evidence = [e for e in cases if e['oracle'] == 'executed' and e['instruction'].split()[0].upper() in matched]
        if 'INS' in names:
            evidence = [e for e in evidence if e['family'] == 'SIMD_copy']
        if 'NOT' in names:
            evidence = [e for e in evidence if e['family'] == 'SIMD_2misc']
        failing = [e for e in evidence if e['mismatches']]
        optional = any(n.startswith(('AES', 'SHA1', 'SHA256')) for n in names)
        profile = 'optional-crypto' if optional else 'base-8bit;optional-64bit' if 'PMULL' in names else 'base'
        if names & UNSUPPORTED:
            status = 'machine-operation-unsupported'
        elif names & PROXIES:
            status = 'runtime-proxy-or-system-review'
        elif failing:
            status = 'counterexample-in-mnemonic-family'
        elif evidence:
            status = 'sampled-family-no-difference;not-all-forms'
        else:
            status = 'no-dynamic-evidence;review-required'
        rows.append(
            [
                section,
                page,
                description,
                profile,
                status,
                len(evidence),
                len(failing),
                ','.join(sorted({e['family'] for e in evidence})) or '-',
                ','.join(e['word'] for e in failing[:3]) or '-',
            ]
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('w') as stream:
        writer = csv.writer(stream, delimiter='\t', lineterminator='\n')
        writer.writerow(
            [
                'section',
                'pdf_page',
                'description',
                'A57_profile',
                'status',
                'sampled_family_words',
                'failing_family_words',
                'decoder_families',
                'example_words',
            ]
        )
        writer.writerows(rows)
    print(f'{len(rows)} instruction-description entries written to {output}')


if __name__ == '__main__':
    main()
