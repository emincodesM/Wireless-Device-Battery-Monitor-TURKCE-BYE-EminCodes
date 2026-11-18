# Değişiklik Günlüğü (Changelog)

Bu projedeki tüm önemli değişiklikler bu dosyada belgelenmektedir. Biçim [Keep a Changelog](https://keepachangelog.com/tr/1.0.0/) standardına dayanır.

## [1.0.8] - 2026-11-18

### Eklenenler (Added)
- **52 Yeni Sürücü ve Protokol Modülü (`1.0.8` Entegrasyonu):**
  - **Kulaklıklar & TWS Kulakiçi:** Apple AirPods / AirPods Pro / AirPods Max & Beats (`airpods.cpp`), Samsung Galaxy Buds / Buds+ / Live / Pro / Buds2 / Buds3 (`galaxy_buds.cpp`), Razer BlackShark V2 Pro / V3 Pro / Kraken V3 / V4 (`razer_headset.cpp`), Razer Barracuda Pro (`barracuda_pro.cpp`), Logitech G435 `046D:0ADF` (`logitech_g435_0adf.cpp`), SteelSeries Arctis Nova Elite (`steelseries_elite.cpp`), ASTRO A50 Gen 4 (`astro.cpp`), Audeze Maxwell (`audeze.cpp`), Fractal Scape (`fractal.cpp`), Jabra Evolve / Engage (`jabra.cpp`), Plantronics / Poly (`plantronics.cpp`), JBL Quantum (`jbl.cpp`), Maono (`maono.cpp`), Moondrop (`moondrop.cpp`), MCHOSE kulaklıklar (`mchose_headset.cpp`), ASUS ROG Pelta & ROG Delta II (`rog.cpp`), HyperX Cloud Stinger Core & Cloud Core DTS (`hyperx.cpp`).
  - **Fareler:** Pulsar (`pulsar.cpp`), WLmouse (`wlmouse.cpp`), Finalmouse (`finalmouse.cpp`), VAXEE (`vaxee.cpp`), Endgame Gear XM2w/OP1w (`endgame.cpp`, `endgame_we.cpp`), Sprime (`sprime.cpp`), Ninjutso (`ninjutso.cpp`), Darmoshark (`darmoshark.cpp`), Fantech (`fantech.cpp`), Dareu (`dareu.cpp`), MCHOSE (`mchose.cpp`, `mchose_modern.cpp`, `mchose_g3.cpp`), ATK BitMouse (`atk_bitmouse.cpp`), ATK Zero / Blazing Sky V2 Nordic 54L15 64-bayt HID (`atk_nordic54.cpp`), Alienware (`alienware.cpp`), Cherry (`cherry.cpp`), MSI (`msi.cpp`), NZXT (`nzxt.cpp`), Areson (`areson.cpp`), Machenike (`machenike.cpp`), Niceboy (`niceboy.cpp`), HyperX Pulsefire Haste / Haste 2 Wireless (`hyperx.cpp`).
  - **Klavyeler:** ATK RS7 / V75 / Z65 klavyeler (`atk_keyboard.cpp`), Royal Kludge (`royal_kludge.cpp`), Semico (`semico.cpp`), Sonix (`sonix.cpp`), Arbiter Polar (`arbiter.cpp`), genişletilmiş AULA F75 / F87 / F99 / F108 (`aula.cpp`) ve Keychron Q/V/K Max (`keychron.cpp`) desteği.
  - **Oyun Kolları (Gamepad) & Telefon:** Nintendo Switch Pro / Joy-Con (`switch_pro.cpp`), Valve Steam Controller (`steam_controller.cpp`), 8BitDo (`eightbitdo.cpp`), Flydigi Vader / Apex (`flydigi.cpp`), Beitong (`beitong.cpp`), Microsoft Phone Link telefon pil durumu (`phonelink.cpp`).
- **Uygulama İçi Ayarlar Paneli (`flyout.cpp`):** Flyout üst barındaki `⚙` düğmesi ile açılan etkileşimli ayarlar görünümü (güncelleme sıklığı, düşük pil eşiği, bildirim anahtarları, başlangıçta çalıştırma, dil seçimi ve gizli cihazları sıfırlama).
- **Kart Üzerinde Mini Pil Grafiği (Sparkline) ve Hızlı Gizleme (`👁`):** Son 30 pil ölçümünden mini eğilim çizgisi ve kart sağ üst köşesinden görev çubuğu görünürlüğünü tek tıkla açıp kapatma.
- **Çoklu Dil Desteği (`i18n`):** Varsayılan Türkçe (`tr`) arayüzün yanı sıra İngilizce (`en`) ve 12 ek dil desteği (`lang/*.txt`).
- **Modüler Mimari:** `src/settings.cpp`, `src/battery_model.cpp`, `src/tray.cpp`, `src/brands.cpp`, `src/labels.cpp` ve `src/i18n.cpp` ayrımı.
- **Birim Testleri (`tests/`):** `test_rapoo_protocol.cpp` ve `test_rapoo_driver.cpp` ile Rapoo, ATK Serial, HyperX Cloud III S, ATK Nordic 54, Galaxy Buds, Logitech G435, Ardor Fury V2, Xenta, `MergeByKey` ve `BatteryModel` testleri.

### Düzeltilenler (Fixed)
- **HyperX Cloud III S Wireless (`03F0:06BE`, `03F0:02CC`):** `0x01C0` kullanım sayfası filtresi (`IsCloud3SCollection`), çevrimdışı `0xFF` kontrolü ve tam şarj (`%100`) geri dönüşü düzeltildi.
- **ATK Neptune N9 Ultra / Pro (`atk_serial.cpp`):** Model tabanlı sabit anahtar (`atk-serial:N9Ultra`), kablo + dongle çift kart birleştirme (`MergeByKey`) ve bozuk çerçevelerde 1-bayt kayan pencere senkronizasyonu (`ParseAtkSerialFrames`) eklendi.
- **ASUS ROG (`rog.cpp`):** `ctx.Claim` çağrısının ilk koleksiyonda diğer koleksiyonları atlamasına yol açan erken kilitleme hatası giderildi.
- **Flyout ve Sağ Tık İndeks Eşleşmesi (`P0-1`) & Yeniden Açılma Yarışı (`P0-2`):** Cihaz sırası değiştirildiğinde yanlış cihaz menüsünün açılması ve tepsi ikonuna tıklayınca kapanıp anında tekrar açılması düzeltildi.
- **HeadsetControl Güvenliği (`P0-4`):** Yalnızca uygulama dizinindeki `headsetcontrol.exe` çalıştırılacak şekilde sınırlandırıldı ve `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` ile tanıtıcı (handle) mirası kısıtlandı.

## [1.0.0] - 2026-11-15
- İlk Türkçe sürüm, Modern Windows 11 Flyout paneli, deşarj hızı/süre tahmini ve yerli Win32 kurulum sihirbazı.
