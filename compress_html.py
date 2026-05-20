#!/usr/bin/env python3
import gzip
import os

def main():
    script_dir = os.path.dirname(os.path.realpath(__file__))
    html_path = os.path.join(script_dir, 'house_lighting_system', 'index.html')
    header_path = os.path.join(script_dir, 'house_lighting_system', 'web_ui.h')

    if not os.path.exists(html_path):
        print(f"Error: index.html not found at {html_path}")
        return

    print(f"Reading {html_path}...")
    with open(html_path, 'r', encoding='utf-8') as f:
        html_content = f.read()

    # Minify simple spaces (we don't need full HTML minifier, but basic line minification helps)
    minified_lines = []
    for line in html_content.splitlines():
        line = line.strip()
        if line.startswith('//') or line.startswith('/*') or not line:
            # Skip empty lines or pure single-line JS comments to save bytes
            continue
        minified_lines.append(line)
    minified_html = '\n'.join(minified_lines)

    print(f"Original size: {len(html_content)} bytes")
    print(f"Minified size: {len(minified_html)} bytes")

    print("Compressing using gzip...")
    compressed_bytes = gzip.compress(minified_html.encode('utf-8'))
    compressed_len = len(compressed_bytes)
    print(f"Gzipped size: {compressed_len} bytes")

    # Generate web_ui.h content
    header_content = []
    header_content.append("// This file is auto-generated. Do not edit directly.")
    header_content.append("#ifndef WEB_UI_H")
    header_content.append("#define WEB_UI_H")
    header_content.append("")
    header_content.append("#include <pgmspace.h>")
    header_content.append("")
    header_content.append(f"const size_t INDEX_HTML_GZ_LEN = {compressed_len};")
    header_content.append("")
    header_content.append("const uint8_t INDEX_HTML_GZ[] PROGMEM = {")

    # Format bytes as hex grid
    hex_bytes = [f"0x{b:02x}" for b in compressed_bytes]
    for i in range(0, len(hex_bytes), 12):
        chunk = hex_bytes[i:i+12]
        header_content.append("  " + ", ".join(chunk) + ("," if i + 12 < len(hex_bytes) else ""))

    header_content.append("};")
    header_content.append("")
    header_content.append("#endif // WEB_UI_H")

    with open(header_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(header_content) + '\n')

    print(f"Header file successfully created: {header_path}")

if __name__ == '__main__':
    main()
