# NootedRed ![GitHub Workflow Status](https://img.shields.io/github/actions/workflow/status/ChefKissInc/NootedRed/main.yml?branch=master&logo=github&style=for-the-badge) [![Release Badge](https://img.shields.io/github/v/release/ChefKissInc/NootedRed?include_prereleases&style=for-the-badge&sort=semver&color=blue)](https://github.com/ChefKissInc/NootedRed/releases) [![Downloads Badge](https://img.shields.io/github/downloads/ChefKissInc/NootedRed/total?style=for-the-badge)](https://github.com/ChefKissInc/NootedRed/releases/latest)

## about my fork
this fork focuses on fixes for specific issues i encountered with this kext, for my own hardware (a picasso 3500U APU). i don't intend to distribute this as an alternative development for NootedRed since i have almost no OS/kernel programming knowledge of my own, and the fixes obtained are through use of AI which the original project does not endorse (i'm only learning about mac kernel development if anything). so far i have achieved:
- fixed backlight adjustment (fixed AMD ABM/Vari-Bright interference)
- fixed sleep wake (obtained via disabling `surpriseRemoval` trigger for late GPU entry)

i will try my best to see how far i can go with further stabilizing this kext for my machine and document the findings here. now for the rest of the original README, see below.

---

> [!CAUTION]
> Please consider donating: donations help me stay alive and pay costs; if the Hackintosh projects don't receive funding, I will (have to) stop working on them.
>
> PayPal or credit/debit card: https://ko-fi.com/chefkiss
>
> BTC: `bc1qgu56kptepex2csuzl5nhzc4vxuj8c6ggjzhcem`
>
> SOL: `4PJU3iB5rimN9BNcpzduvmTnptJyedZiAd23so33SAdi`
>
> ETH: `0x038A25849c23Bc5A736484351a8B6Ad71bC46676`

The macOS kernel extension for GPU bug fixes, QoL improvements, and extended hardware support.

```
Copyright (C) 2022-2026  ChefKiss

Licensed under the ChefKiss Open Knowledge License version 1.

This work is licensed to you under the terms of the ChefKiss Open Knowledge
License, either version 1 of the License, or (at your option) any later
version published by ChefKiss.

This work may not be used to train or develop artificial intelligence,
and may not be used with artificial intelligence in any other way. See
the ChefKiss Open Knowledge License for details.

This work is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
ChefKiss Open Knowledge License for more details.

You should have received a copy of the ChefKiss Open Knowledge License
along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.
```

> [!WARNING]
> This project is under active research and development; There will be crashes here and there, as the project is glue between the existing buggy AMD drivers.
>
> See repository issues and [our site](https://chefkiss.dev/applehax/nootedred) for more information.
