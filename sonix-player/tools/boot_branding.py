#!/usr/bin/env python3
"""Fit approved artwork to each panel and encode baseline JPEG boot assets."""
from pathlib import Path
from PIL import Image, ImageOps


def main():
    root = Path(__file__).resolve().parents[2]
    source = root / 'sonix-player/assets/branding/boot-rekkod.png'
    with Image.open(source) as image:
        image = image.convert('RGB')
        for model, height in [('R1', 800), ('R3PII', 720)]:
            fitted = ImageOps.fit(image, (480, height), Image.Resampling.LANCZOS)
            directory = root / 'sonix-packer/assets' / model / 'etc'
            # A dark atmospheric identity in every early-boot theme slot.
            # Theme selection and the boot display program remain unchanged.
            for name in ['logo.jpeg', 'logo1.jpeg', 'logo2.jpeg']:
                target = directory / name
                fitted.save(target, 'JPEG', quality=94, subsampling=0, progressive=False)
                with Image.open(target) as check:
                    assert check.mode == 'RGB' and check.size == (480, height)
                    assert not check.info.get('progressive')
                print(f'{model}/{name}: 480x{height}, baseline JPEG')


if __name__ == '__main__':
    main()
