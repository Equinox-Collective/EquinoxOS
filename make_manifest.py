# make_manifest.py
import os

ISO_ROOT = "iso_root"
manifest_path = os.path.join(ISO_ROOT, "etc", "install_manifest.txt")

# Создаем папку etc, если её нет
os.makedirs(os.path.dirname(manifest_path), exist_ok=True)

with open(manifest_path, "w", newline="\n", encoding="utf-8") as f:
    for root, dirs, files in os.walk(ISO_ROOT):
        for fname in files:
            fpath = os.path.join(root, fname)
            # Вычисляем относительный путь, заменяя виндовые слэши на юниксовые
            rel_path = os.path.relpath(fpath, ISO_ROOT).replace("\\", "/")
            
            # Нам не нужно записывать сам манифест, файлы загрузчика и EFI
            if "etc/install_manifest.txt" in rel_path:
                continue
            if rel_path.startswith("boot/") or rel_path.startswith("EFI/"):
                continue
                
            f.write(f"/{rel_path}\n")

print("[MANIFEST] Successfully generated etc/install_manifest.txt")
