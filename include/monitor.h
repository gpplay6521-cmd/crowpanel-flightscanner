#pragma once

// Remote screen monitor (see "ekran izleme" tool): once Wi-Fi is up it serves
//   GET /screenshot.bmp   the picture currently on the panel
//   GET /status           boot/reset info, heap, plane count, last error
//   GET /action?name=tap&value=x,y    simulate a touch at x,y
//   GET /action?name=refresh          fetch new traffic now
//   GET /reboot
void monitorBegin();
