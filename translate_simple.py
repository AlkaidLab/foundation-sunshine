import os
import re
import requests

# 项目名和术语保护列表
PROTECTED_TERMS = [
    'Sunshine', 'README', 'GitHub', 'CI', 'API', 'Markdown', 'OpenAI', 'DeepL', 'Google Translate',
    # 可在此添加更多术语
]

def mask_terms(text):
    for term in PROTECTED_TERMS:
        text = re.sub(rf'(?<![`\w]){re.escape(term)}(?![`\w])', f'@@@{term}@@@', text)
    return text

def unmask_terms(text):
    for term in PROTECTED_TERMS:
        text = text.replace(f'@@@{term}@@@', term)
    return text

def translate_with_deepseek(text, target_lang):
    # 使用 DeepSeek API 进行翻译
    api_key = os.getenv('DEEPSEEK_API_KEY')
    if not api_key:
        raise Exception('DEEPSEEK_API_KEY 环境变量未设置')
    url = 'https://api.deepseek.com/v1/chat/completions'
    prompt = f"请将以下 Markdown 内容翻译为{target_lang}，但不要翻译项目名和术语：{', '.join(PROTECTED_TERMS)}。保持原有格式、链接和图片。\n\n{text}"
    headers = {
        'Authorization': f'Bearer {api_key}',
        'Content-Type': 'application/json'
    }
    payload = {
        "model": "deepseek-chat",
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.2
    }
    resp = requests.post(url, headers=headers, json=payload)
    resp.raise_for_status()
    result = resp.json()
    return result['choices'][0]['message']['content']

def translate_readme():
    with open('README.md', 'r', encoding='utf-8') as f:
        content = f.read()

    languages = [
        ('en', 'English'),
        ('fr', 'French'),
        ('de', 'German'),
        ('ja', 'Japanese')
    ]

    for lang_code, lang_name in languages:
        try:
            if lang_code == 'zh_CN':
                translated_content = content
            else:
                masked = mask_terms(content)
                translated = translate_with_deepseek(masked, lang_name)
                translated = unmask_terms(translated)
                # 去除 DeepSeek 返回的多余提示，只保留第一个 Markdown 标题及后面内容
                lines = translated.splitlines()
                for idx, line in enumerate(lines):
                    if line.strip().startswith('#'):
                      translated_content = '\n'.join(lines[idx:])
                      break
                else:
                  translated_content = translated.strip()

            filename = f'README.{lang_code}.md'
            with open(filename, 'w', encoding='utf-8') as f:
                f.write(translated_content)
            print(f"✓ Translated to {lang_name} ({lang_code})")
        except Exception as e:
            print(f"✗ Failed to translate to {lang_name}: {e}")

if __name__ == "__main__":
    translate_readme()
