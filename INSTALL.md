# SmartComp — Beta Installation

Instructions for beta testers. Deutsche Version unten.

## English

### 1. Copy the plugin

Place `SmartComp.vst3` into your VST3 folder:

```
~/Library/Audio/Plug-Ins/VST3/
```

### 2. Remove quarantine

macOS blocks unsigned plugins. Open Terminal and run:

```bash
sudo xattr -rd com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/SmartComp.vst3
```

Enter your password if prompted.

### 3. If macOS still blocks it

**System Settings** → **Privacy & Security** → at the bottom you should see
that SmartComp was blocked → click **Allow Anyway**

### 4. DAW

Restart Ableton → Plug-Ins → VST3 → SmartComp

**Checklist:**

- [ ] `.vst3` is in the VST3 folder
- [ ] Quarantine removed via Terminal
- [ ] Ableton rescanned or restarted

---

## Deutsch

### 1. Plugin kopieren

Die `SmartComp.vst3` in den VST3-Ordner legen:

```
~/Library/Audio/Plug-Ins/VST3/
```

### 2. Quarantine entfernen

macOS blockiert unsignierte Plugins. Terminal öffnen und eingeben:

```bash
sudo xattr -rd com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/SmartComp.vst3
```

Passwort eingeben, wenn gefragt.

### 3. Falls macOS trotzdem meckert

**Systemeinstellungen** → **Datenschutz & Sicherheit** → unten steht
„SmartComp wurde blockiert" → **Trotzdem erlauben**

### 4. DAW

Ableton neu starten → Plug-Ins → VST3 → SmartComp

**Kurz-Checkliste:**

- [ ] `.vst3` im VST3-Ordner
- [ ] Quarantine per Terminal entfernt
- [ ] Ableton rescan oder Neustart
