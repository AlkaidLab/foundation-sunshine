import os
import re
from googletrans import Translator

def translate_readme():
    # Read the original README
    with open('README.md', 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Initialize translator
    translator = Translator()
    
    # Languages to translate to
    languages = [
        ('en', 'English'),
        ('zh-CN', 'Chinese Simplified'),
        ('fr', 'French'),
        ('de', 'German'),
        ('ja', 'Japanese')
    ]
    
    for lang_code, lang_name in languages:
        try:
            if lang_code == 'en':
                # For English, copy the original content
                translated_content = content
            else:
                # Translate the content
                translated = translator.translate(content, dest=lang_code)
                translated_content = translated.text
            
            # Write translated file
            filename = f'README.{lang_code}.md'
            with open(filename, 'w', encoding='utf-8') as f:
                f.write(translated_content)
            
            print(f"✓ Translated to {lang_name} ({lang_code})")
            
        except Exception as e:
            print(f"✗ Failed to translate to {lang_name}: {e}")

if __name__ == "__main__":
    translate_readme()
