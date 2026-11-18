# Katkıda Bulunma Rehberi (Contributing)

**Kablosuz Cihaz Pil İzleyici (Wireless Device Battery Monitor)** projesine katkıda bulunmak istediğiniz için teşekkürler!

## 1. Yeni Bir Cihaz Desteği İsteme (`--dump`)

Cihazınız listede görünmüyorsa veya pil yüzdesi yanlış okunuyorsa:
1. Cihazınızı bilgisayarınıza bağlayın ve açık olduğundan emin olun.
2. Sistem tepsisindeki pil simgesine sağ tıklayıp **"Cihaz Tanılama Raporu (--dump)"** seçeneğine tıklayın (veya `WirelessBattery.exe --dump` komutunu çalıştırın).
3. Masaüstünde oluşan `wireless-battery-dump.txt` dosyasını [Yeni Cihaz Desteği Talebi](.github/ISSUE_TEMPLATE/device_request.md) şablonuyla yeni bir Issue olarak paylaşın.

## 2. Yeni Bir Sürücü Geliştirme Kuralları

Yeni bir sürücü (`src/drivers/<marka>.cpp`) eklerken aşağıdaki güvenlik ve kalite kurallarına uyulmalıdır:
- **Yalnızca Pil Sorgusu:** Cihaza yalnızca pil yüzdesi / voltaj / şarj durumu okuyan komutlar gönderilmelidir. DPI, aydınlatma (RGB), raporlama hızı (polling rate) veya alıcı eşleştirmesini (pairing) değiştiren komutlar **kesinlikle gönderilmemelidir**.
- **Standart HID Koleksiyonlarına Dokunmayın:** Yalnızca üreticiye özel (`UsagePage >= 0xFF00` veya belgelenmiş özel sayfa) koleksiyonlara yazın; standart fare (`01:02`) veya klavye (`01:06`) koleksiyonlarına ham paket göndermeyin.
- **Zaman Aşımı Sınırı:** Uyuyan veya kapalı bir cihaz yoklama iş parçacığını (poll thread) kilitlememelidir. Her okuma çağrısı (`d.Read`) kısa bir zaman aşımı (`150–500 ms`) kullanmalı ve cihaz başına toplam süre `4 saniye`yi aşmamalıdır.
- **Birim Testi:** Saf protokol ayrıştırıcısını `src/drivers/<marka>_protocol.h` dosyasına ayırın ve `tests/test_rapoo_protocol.cpp` içerisine test senaryosu ekleyin (`make test`).

Adım adım kod şablonu için [`docs/ADDING_A_DRIVER.md`](docs/ADDING_A_DRIVER.md) belgesini inceleyebilirsiniz.
