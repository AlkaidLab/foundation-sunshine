#!/usr/bin/env python3
"""Regenerate translated READMEs into docs/readme/ from the root README.md."""
import argparse, os, re
import requests

PROTECTED_TERMS = ['Foundation Sunshine', 'Sunshine', 'Moonlight', 'README', 'GitHub', 'CI', 'API', 'HDR', 'HDR10+', 'HDR Vivid', 'Dolby Vision', 'NVENC', 'AMF', 'Quick Sync', 'DualSense', 'ZakoDisplay', 'Zako Direct', 'Windows', 'macOS', 'Linux', 'Android', 'HarmonyOS', 'iOS', 'iPadOS', 'Steam', 'Epic', 'GOG']
LANG_NAMES = {'de': 'German', 'fr': 'French', 'ja': 'Japanese', 'es': 'Spanish', 'pt': 'Portuguese', 'ru': 'Russian', 'it': 'Italian', 'ko': 'Korean', 'cs': 'Czech', 'sv': 'Swedish', 'tr': 'Turkish'}
SWITCHER = '[简体中文](../../README.md) · [English](README.en.md) · [Deutsch](README.de.md) · [Français](README.fr.md) · [日本語](README.ja.md)'

def mask(text):
    for term in PROTECTED_TERMS:
        text = re.sub(rf'(?<![\w`]){re.escape(term)}(?![\w`])', f'@@@{term}@@@', text)
    return text

def unmask(text):
    for term in PROTECTED_TERMS:
        text = text.replace(f'@@@{term}@@@', term)
    return text

def translate(text, target):
    key = os.environ.get('DEEPSEEK_API_KEY')
    if not key:
        raise SystemExit('DEEPSEEK_API_KEY is not set')
    prompt = (f'Translate the following Markdown into {target}. Keep every Markdown link, image, table and heading anchor working. '
              f'Do not translate these terms: {", ".join(PROTECTED_TERMS)}.'
              + chr(10)*2 + text)
    r = requests.post('https://api.deepseek.com/v1/chat/completions',
                      headers={'Authorization': f'Bearer {key}', 'Content-Type': 'application/json'},
                      json={'model': 'deepseek-chat', 'messages': [{'role': 'user', 'content': prompt}], 'temperature': 0.2},
                      timeout=180)
    r.raise_for_status()
    return r.json()['choices'][0]['message']['content']

def localize_paths(markdown):
    markdown = markdown.replace('src="assets/readme/', 'src="../../assets/readme/')
    markdown = markdown.replace('srcset="assets/readme/', 'srcset="../../assets/readme/')
    markdown = markdown.replace('](docs/zh-CN/', '](../en/')
    markdown = markdown.replace('](docs/contributing.md)', '](../contributing.en.md)')
    return markdown

def fix_switcher(markdown):
    lines = markdown.splitlines()
    for i, line in enumerate(lines[:12]):
        if '简体中文' in line and 'English' in line:
            lines[i] = SWITCHER
            break
    else:
        lines.insert(1, '')
        lines.insert(2, SWITCHER)
    return chr(10).join(lines)

def strip_broken_anchors(markdown):
    anchors = {re.sub(r'[^\w\- ]', '', h.lower()).replace(' ', '-') for h in re.findall(r'^#{1,6} (.+)$', markdown, re.M)}
    def repl(m):
        label, target = m.group(1), m.group(2)
        return f'[{label}](#{target})' if target in anchors else label
    return re.sub(r'\[([^\]]+)\]\(#([^)]+)\)', repl, markdown)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--languages', default='de,fr,ja')
    args = ap.parse_args()
    source = open('README.md', encoding='utf-8').read()
    out_dir = os.path.join('docs', 'readme')
    os.makedirs(out_dir, exist_ok=True)
    for code in [c.strip() for c in args.languages.split(',') if c.strip()]:
        name = LANG_NAMES.get(code)
        if not name:
            print(f'skip unknown language: {code}')
            continue
        translated = translate(mask(source), name)
        translated = strip_broken_anchors(localize_paths(fix_switcher(unmask(translated))))
        with open(os.path.join(out_dir, f'README.{code}.md'), 'w', encoding='utf-8') as f:
            f.write(translated.strip() + chr(10))
        print(f'written docs/readme/README.{code}.md')

if __name__ == '__main__':
    main()
