# Yeni Bir Sürücü Ekleme Rehberi (Adding a Driver)

Bu rehber, **Wireless Device Battery Monitor** projesine yeni bir kablosuz fare, klavye, kulaklık veya oyun kolu sürücüsü eklemek için izlenmesi gereken adımları açıklar.

## 1. Cihazın HID / USB Yapısını İnceleme (`--dump`)

Öncelikle cihaz bağlıyken tanılama çıktısını alın:

```powershell
.\WirelessBattery.exe --dump
```

`wireless-battery-dump.txt` dosyasında cihazınızın `VID:PID` değerini, `page` (Usage Page), `usage`, `in` (Input Report uzunluğu), `out` (Output Report uzunluğu) ve `feat` (Feature Report uzunluğu) değerlerini kontrol edin:

```text
03F0:06BE  page=01C0 usage=0001  in=64 out=64 feat= 0  "HyperX Cloud III S Wireless"
```

## 2. Sürücü Dosyasını Oluşturma (`src/drivers/<marka>.cpp`)

1. Yalnızca pil okuma komutu gönderin; **asla** DPI, RGB, polling rate veya eşleştirme (pairing) değiştiren komutlar göndermeyin.
2. Cihaz yanıt vermediğinde sonsuz döngüye girmeyin; `d.Read(r, timeoutMs)` için `150`–`500 ms` arası zaman aşımı kullanın ve her cihaz için toplam süreyi `4 saniye` altında tutun.
3. Cihazı tanıdıktan sonra `ctx.Add(s, vid, pid)` veya çift bağlantılı (kablo + dongle) cihazlar için `MergeByKey(ctx, s, vid, pid)` çağırarak genel yedek (fallback) sürücülerin aynı cihazı tekrar eklemesini önleyin.

Örnek şablon:

```cpp
#include "drivers.h"

void PollMyVendor(PollContext& ctx) {
    constexpr USHORT kVid = 0x1234;
    constexpr USHORT kPid = 0x5678;
    if (ctx.IsClaimed(kVid, kPid)) return;

    for (const HidColl* c : ctx.Find(kVid, kPid, [](const HidColl& c) {
             return c.page() >= 0xFF00 && c.outLen() >= 16 && c.inLen() >= 16;
         })) {
        HidDev d;
        if (!d.Open(*c)) continue;
        d.Flush();
        if (!d.Write({ 0x01, 0x02, 0x03 })) continue;
        std::vector<BYTE> r;
        if (d.Read(r, 250) < 6) continue;

        DeviceStatus s;
        s.key = L"myvendor:" + Hex4(kPid);
        s.name = L"MyVendor Wireless Mouse";
        s.kind = DeviceKind::Mouse;
        s.via = L"2.4 GHz receiver";
        s.percent = Clamp100(r[4]);
        s.charging = r[5] == 1;
        ctx.Add(s, kVid, kPid);
        return;
    }
}
```

## 3. Sürücüyü Kaydetme

1. `src/drivers/drivers.h` dosyasına `void PollMyVendor(PollContext& ctx);` bildirimini ekleyin.
2. `src/devices.cpp` içindeki `PollAllDevices()` fonksiyonunda (genel fallback sürücülerinden **önce**) `PollMyVendor(ctx);` çağrısını ekleyin.
3. Gerekirse `src/devices.cpp` içindeki `BrandPrefix(unsigned short vid)` tablosuna üreticinin USB Vendor ID değerini ekleyin.
4. Saf protokol çözümleyicisini `src/drivers/<marka>_protocol.h` başlığına ayırıp `tests/test_rapoo_protocol.cpp` dosyasına birim testi ekleyin.
