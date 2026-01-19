ARC_LCD_ base mini cluster for HYUNDAI GENESIS 2008~2013
UI based LVGL, ESP32 MCU based.

now latest version : V1.3.0

제 차량에는 어댑티브 스마트 크루즈 컨트롤이 있습니다.
작동중일 시에 계기판에 작동상황을 이미지로 함께 보여주죠.
![genesis_scc_cluster](https://github.com/user-attachments/assets/fabd4b33-eb65-4711-8534-eb37cedda82c)

하지만, 이 기능을 키고 주행하면서 전방상황과 계기판을 번갈아가며 보아야 하는 불편함을 느꼈습니다.
요즈음 차량에 거의 장착되는 헤드업 디스플레이는 전면유리창에 필요한 정보들을 보여주기 때문에 계기판까지 시야를 내릴 필요가 없죠.
이에, 속도 및 크루즈컨트롤의 동작상태를 확인하는 보조모니터를 제작해 보고 싶어졌습니다.
//(CAN 해석을 통해 아두이노 및 OLED 디스플레이를 이용한 보조모니터는 다른 글을 참고하세요)

원형LCD디스플레이와 ESP32를 이용한 소형계기판입니다.
LVGL ui 엔진을 이용하였습니다.
![LVGL_ui](https://github.com/user-attachments/assets/91ed1307-1255-4086-a01a-28baaac24707)
![ui_test_02](https://github.com/user-attachments/assets/0c8ba80b-b0cc-4686-bdf7-4e289b294301)

실주행을 통해 버그를 수정하고 기능을 개선중입니다.

![esp_test_02](https://github.com/user-attachments/assets/7726f7f0-4eb9-4f52-99b9-da16c5a0a189)
![esp_test_01](https://github.com/user-attachments/assets/c97f5421-b30c-4963-9473-9680c2d50a51)


*사용제품
  1. ESP32 (MCU)
  2. MCP2515 (can 수신 모듈)
  3. GC981A01 (LCD디스플레이)
  4. 기타 배선과 브레드보드 등

*기능
  1. 부팅화면(오프닝)
  2. 현재속도의 수치 및 아크게이지
  3. 오토홀드 동작상태
  4. 변속기위치상태
  5. 냉각수온 아크게이지 (고온시 경고)
  6. 어댑티브 크루즈 동작화면 (전방차량 유무와 차간거리, 세팅속도)
  7. 나이트모드 --> (개발중)
  8. 스포츠모드 --> (ui스크린 구현완료)
![KakaoTalk_20260109_160729640-ezgif com-video-to-gif-converter (1)](https://github.com/user-attachments/assets/0147e518-0e0e-47d1-993a-f31694beb3fd)



*현재까지 발견된 문제
  1. 고속 일 수록 속도계의 편차가 있음
  2. 모듈이 너무 큼
