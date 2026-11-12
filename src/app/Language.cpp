#include "Language.h"

#include "dui/Utils/StringConvert.h"

#include <cstring>

namespace sdk {

namespace {

// All strings are stored as UTF-8 narrow literals and converted to std::string on
// lookup (std::string is wstring on UNICODE Windows builds, UTF-8 string elsewhere;
// ui::StringConvert handles both).
struct Entry {
    const char* key;
    const char* zhCN;
    const char* zhTW;
    const char* en;
    const char* ja;
    const char* ko;
    const char* de;
    const char* fr;
    const char* ru;
};

// Order of the 8 columns must match enum class Lang.
#define E(key, zh, tw, en, ja, ko, de, fr, ru) \
    { key, zh, tw, en, ja, ko, de, fr, ru }

const Entry kEntries[] = {
    // ---- App / common ----
    E("app.title", "StarDesk", "StarDesk", "StarDesk", "StarDesk", "StarDesk", "StarDesk", "StarDesk", "StarDesk"),
    E("app.subtitle", "远程控制", "遠程控制", "Remote Desktop", "リモートデスクトップ", "원격 데스크톱", "Fernsteuerung", "Bureau à distance", "Удаленный рабочий стол"),
    E("common.ok", "确定", "確定", "OK", "OK", "확인", "OK", "OK", "ОК"),
    E("common.cancel", "取消", "取消", "Cancel", "キャンセル", "취소", "Abbrechen", "Annuler", "Отмена"),
    E("common.on", "开", "開", "On", "オン", "켜기", "An", "Activé", "Вкл"),
    E("common.off", "关", "關", "Off", "オフ", "끄기", "Aus", "Désactivé", "Выкл"),
    E("window.min", "最小化", "最小化", "Minimize", "最小化", "최소화", "Minimieren", "Réduire", "Свернуть"),
    E("window.max", "最大化", "最大化", "Maximize", "最大化", "최대화", "Maximieren", "Agrandir", "Развернуть"),
    E("window.restore", "还原", "還原", "Restore", "元に戻す", "복원", "Wiederherstellen", "Restaurer", "Восстановить"),
    E("window.close", "关闭", "關閉", "Close", "閉じる", "닫기", "Schließen", "Fermer", "Закрыть"),

    // ---- Host (left panel) ----
    E("host.device", "本机设备", "本機設備", "This Device", "このデバイス", "이 기기", "Dieses Gerät", "Cet appareil", "Это устройство"),
    E("host.ip", "本机地址", "本機地址", "Local Address", "ローカルアドレス", "로컬 주소", "Lokale Adresse", "Adresse locale", "Локальный адрес"),
    E("host.password", "连接密码", "連接密碼", "Password", "接続パスワード", "연결 비밀번호", "Passwort", "Mot de passe", "Пароль"),
    E("host.genPassword", "随机", "隨機", "Random", "ランダム", "랜덤", "Zufall", "Aléatoire", "Случайный"),
    E("host.showPassword", "显示", "顯示", "Show", "表示", "표시", "Anzeigen", "Afficher", "Показать"),
    E("host.hidePassword", "隐藏", "隱藏", "Hide", "隠す", "숨기기", "Verbergen", "Masquer", "Скрыть"),
    E("host.acceptMode", "连接方式", "連接方式", "Connection", "接続方式", "연결 방식", "Verbindung", "Connexion", "Подключение"),
    E("host.autoAccept", "自动接受(密码正确即可连接)", "自動接受(密碼正確即可連接)", "Auto accept (password check only)", "自動承認(パスワードで接続)", "자동 수락(비밀번호로 연결)", "Automatisch akzeptieren (nur Passwort)", "Accepter automatiquement (mot de passe)", "Принимать автоматически (по паролю)"),
    E("host.manualAccept", "每次手动确认", "每次手動確認", "Ask me each time", "毎回確認する", "매번 수동 확인", "Jedes Mal fragen", "Demander à chaque fois", "Спрашивать каждый раз"),
    E("host.shareMode", "共享屏幕", "共享屏幕", "Share Screen", "画面共有", "화면 공유", "Bildschirm teilen", "Partager l'écran", "Демонстрация экрана"),
    E("host.shareMirror", "复制(镜像主屏)", "複製(鏡像主屏)", "Mirror (primary display)", "複製(メインディスプレイ)", "복제(주 화면)", "Spiegeln (Hauptbildschirm)", "Miroir (écran principal)", "Дублирование (основной экран)"),
    E("host.shareExtend", "拓展(拼接所有屏幕)", "拓展(拼接所有屏幕)", "Extend (span all displays)", "拡張(全ディスプレイを結合)", "확장(모든 화면 결합)", "Erweitern (alle Bildschirme)", "Étendre (tous les écrans)", "Расширение (все экраны)"),
    E("host.status", "状态", "狀態", "Status", "ステータス", "상태", "Status", "État", "Статус"),
    E("host.ready", "就绪,等待连接", "就緒,等待連接", "Ready, waiting for connection", "接続待機中", "연결 대기 중", "Bereit, warte auf Verbindung", "Prêt, en attente de connexion", "Готов, жду подключения"),
    E("host.connected", "已连接", "已連接", "Connected", "接続済み", "연결됨", "Verbunden", "Connecté", "Подключено"),
    E("host.sessionOf", "%s 正在连接本机", "%s 正在連接本機", "%s is connecting", "%s が接続中", "%s 연결 중", "%s verbindet sich", "%s se connecte", "%s подключается"),
    E("host.fileSaveDir", "文件接收目录", "文件接收目錄", "Receive Folder", "受信フォルダ", "받는 폴더", "Empfangsordner", "Dossier de réception", "Папка приема"),
    E("host.captureError", "画面采集失败,已切换为测试画面:%s", "畫面採集失敗,已切換為測試畫面:%s", "Screen capture failed, switched to test pattern: %s", "画面の取得に失敗しました。テストパターンに切り替えました: %s", "화면 캡처 실패, 테스트 패턴으로 전환됨: %s", "Bildschirmaufnahme fehlgeschlagen, Testbild aktiviert: %s", "Échec de la capture, image de test activée : %s", "Ошибка захвата экрана, переключено на тестовое изображение: %s"),

    // ---- Client (right panel) ----
    E("client.title", "远程控制", "遠程控制", "Remote Control", "リモートコントロール", "원격 제어", "Fernsteuerung", "Contrôle à distance", "Удаленное управление"),
    E("client.devices", "局域网设备", "局域網設備", "LAN Devices", "LAN デバイス", "LAN 장치", "LAN-Geräte", "Appareils LAN", "Устройства в сети"),
    E("client.ip", "对方 IP", "對方 IP", "Remote IP", "接続先 IP", "상대 IP", "Ziel-IP", "IP distante", "IP устройства"),
    E("client.port", "端口", "端口", "Port", "ポート", "포트", "Port", "Port", "Порт"),
    E("client.password", "密码", "密碼", "Password", "パスワード", "비밀번호", "Passwort", "Mot de passe", "Пароль"),
    E("client.mode", "模式", "模式", "Mode", "モード", "모드", "Modus", "Mode", "Режим"),
    E("client.modeControl", "控制", "控制", "Control", "操作", "제어", "Steuern", "Contrôler", "Управление"),
    E("client.modeView", "观看", "觀看", "View only", "表示のみ", "보기 전용", "Nur ansehen", "Affichage seul", "Только просмотр"),
    E("client.resolution", "分辨率", "解析度", "Resolution", "解像度", "해상도", "Auflösung", "Résolution", "Разрешение"),
    E("client.resOriginal", "原始", "原始", "Original", "元のまま", "원본", "Original", "Original", "Оригинал"),
    E("client.res720p", "720p", "720p", "720p", "720p", "720p", "720p", "720p", "720p"),
    E("client.res1080p", "1080p", "1080p", "1080p", "1080p", "1080p", "1080p", "1080p", "1080p"),
    E("client.fps", "帧率", "幀率", "Frame rate", "フレームレート", "프레임 속도", "Bildrate", "Images/s", "Частота кадров"),
    E("client.fps24", "24 帧", "24 幀", "24 fps", "24 fps", "24fps", "24 fps", "24 i/s", "24 кадр/с"),
    E("client.fps30", "30 帧", "30 幀", "30 fps", "30 fps", "30fps", "30 fps", "30 i/s", "30 кадр/с"),
    E("client.fps60", "60 帧", "60 幀", "60 fps", "60 fps", "60fps", "60 fps", "60 i/s", "60 кадр/с"),
    E("client.connect", "连接", "連接", "Connect", "接続", "연결", "Verbinden", "Connecter", "Подключить"),
    E("client.disconnect", "断开", "斷開", "Disconnect", "切断", "연결 끊기", "Trennen", "Déconnecter", "Отключить"),
    E("client.connecting", "正在连接…", "正在連接…", "Connecting…", "接続中…", "연결 중…", "Verbinde…", "Connexion…", "Подключение…"),
    E("client.connected_", "已连接", "已連接", "Connected", "接続済み", "연결됨", "Verbunden", "Connecté", "Подключено"),
    E("client.waitingAccept", "等待对方接受…", "等待對方接受…", "Waiting for acceptance…", "相手の承認待ち…", "상대 수락 대기 중…", "Warte auf Bestätigung…", "En attente d'acceptation…", "Ожидание подтверждения…"),
    E("client.authFailed", "密码错误或连接被拒绝", "密碼錯誤或連接被拒絕", "Wrong password or connection rejected", "パスワードが違うか接続が拒否されました", "비밀번호가 틀렸거나 연결이 거부됨", "Falsches Passwort oder Verbindung abgelehnt", "Mot de passe incorrect ou connexion refusée", "Неверный пароль или отказ в подключении"),
    E("client.rejected", "对方拒绝了本次连接", "對方拒絕了本次連接", "Connection rejected by the peer", "接続が拒否されました", "상대가 연결을 거부함", "Verbindung vom Peer abgelehnt", "Connexion refusée par le pair", "Соединение отклонено"),
    E("client.timeout", "连接超时", "連接超時", "Connection timed out", "接続がタイムアウトしました", "연결 시간 초과", "Zeitüberschreitung", "Délai dépassé", "Тайм-аут соединения"),
    E("client.networkError", "网络错误", "網絡錯誤", "Network error", "ネットワークエラー", "네트워크 오류", "Netzwerkfehler", "Erreur réseau", "Сетевая ошибка"),
    E("client.remoteDisconnected", "远端已断开连接", "遠端已斷開連接", "Remote disconnected", "リモートが切断されました", "원격 연결이 끊어짐", "Remote getrennt", "Connexion distante interrompue", "Удаленное соединение разорвано"),
    E("client.wrongVersion", "版本不兼容", "版本不兼容", "Incompatible version", "バージョン不一致", "버전이 호환되지 않음", "Inkompatible Version", "Version incompatible", "Несовместимая версия"),
    E("client.noDevices", "暂未发现局域网设备", "暫未發現局域網設備", "No LAN devices found", "LAN デバイスが見つかりません", "LAN 장치를 찾을 수 없음", "Keine LAN-Geräte gefunden", "Aucun appareil LAN trouvé", "Устройства в сети не найдены"),

    // ---- Remote window ----
    E("remote.viewOnly", "观看模式", "觀看模式", "View only", "表示のみ", "보기 전용", "Nur ansehen", "Affichage seul", "Только просмотр"),
    E("remote.testPattern", "测试画面", "測試畫面", "Test pattern", "テストパターン", "테스트 패턴", "Testbild", "Image de test", "Тестовое изображение"),
    E("remote.sendFile", "发送文件", "發送文件", "Send file", "ファイル送信", "파일 보내기", "Datei senden", "Envoyer un fichier", "Отправить файл"),
    E("remote.fullscreen", "全屏", "全屏", "Fullscreen", "全画面", "전체 화면", "Vollbild", "Plein écran", "Во весь экран"),
    E("remote.exitFullscreen", "退出全屏", "退出全屏", "Exit fullscreen", "全画面を終了", "전체 화면 종료", "Vollbild beenden", "Quitter le plein écran", "Выйти из полноэкранного"),
    E("remote.exitFullscreen", "退出全屏", "退出全屏", "Exit fullscreen", "全画面を終了", "전체 화면 종료", "Vollbild beenden", "Quitter le plein écran", "Выйти из полноэкранного"),
    E("remote.fit", "适应窗口", "適應窗口", "Fit window", "ウィンドウに合わせる", "창에 맞추기", "An Fenster anpassen", "Ajuster à la fenêtre", "По размеру окна"),
    E("remote.cursor", "远程光标", "遠端游標", "Remote cursor", "リモートカーソル", "원격 커서", "Remote-Cursor", "Curseur distant", "Удаленный курсор"),
    E("remote.cursorLocal", "光标：本地", "游標：本機", "Cursor: local", "カーソル：ローカル", "커서: 로컬", "Cursor: lokal", "Curseur : local", "Курсор: локальный"),
    E("remote.cursorDual", "光标：双光标", "游標：雙游標", "Cursor: both", "カーソル：両方", "커서: 둘 다", "Cursor: beide", "Curseur : les deux", "Курсоры: оба"),
    E("remote.cursorDisplay", "显示模式", "顯示模式", "Display mode", "表示モード", "표시 모드", "Anzeigemodus", "Mode affichage", "Режим отображения"),
    E("remote.original", "原始大小", "原始大小", "Actual size", "原寸", "원본 크기", "Originalgröße", "Taille réelle", "Реальный размер"),
    E("remote.disconnect", "断开连接", "斷開連接", "Disconnect", "接続を切断", "연결 끊기", "Trennen", "Déconnecter", "Отключиться"),
    E("remote.fpsLabel", "帧率", "幀率", "FPS", "FPS", "FPS", "FPS", "FPS", "FPS"),
    E("remote.rxFpsLabel", "远端", "遠端", "Remote", "リモート", "원격", "Remote", "Distant", "Удал."),
    E("remote.latencyLabel", "延迟", "延遲", "Latency", "遅延", "지연", "Latenz", "Latence", "Задержка"),
    E("remote.waitingStream", "等待画面…", "等待畫面…", "Waiting for video…", "映像待機中…", "화면 대기 중…", "Warte auf Bild…", "En attente de la vidéo…", "Ожидание изображения…"),
    E("remote.permissionHint", "屏幕录制权限未开启,无法捕获屏幕\n请在 系统设置 > 隐私与安全性 > 屏幕录制 中允许本应用", "屏幕錄製權限未開啟,無法捕獲屏幕\n請在 系統設置 > 隱私與安全性 > 屏幕錄製 中允許本應用", "Screen Recording permission is off; cannot capture the screen\nPlease allow this app in System Settings > Privacy & Security > Screen Recording", "画面収録の権限がありません。画面を取得できません\nシステム設定 > プライバシーとセキュリティ > 画面収録 で許可してください", "화면 녹화 권한이 없어 화면을 캡처할 수 없습니다\n시스템 설정 > 개인정보 및 보안 > 화면 녹화에서 허용하세요", "Keine Bildschirmaufnahme-Berechtigung; Bildschirm kann nicht erfasst werden\nBitte in Systemeinstellungen > Datenschutz & Sicherheit > Bildschirmaufnahme erlauben", "Autorisation d'enregistrement d'écran désactivée; impossible de capturer\nAutorisez l'app dans Réglages Système > Confidentialité et sécurité > Enregistrement d'écran", "Нет разрешения на запись экрана, захват невозможен\nРазрешите приложению доступ в Системные настройки > Конфиденциальность и безопасность > Запись экрана"),
    E("remote.accessibilityHint", "辅助功能权限未开启,无法远程输入\n请在 系统设置 > 隐私与安全性 > 辅助功能 中允许本应用", "輔助功能權限未開啟,無法遠程輸入\n請在 系統設置 > 隱私與安全性 > 輔助功能 中允許本應用", "Accessibility permission is off; remote input is unavailable\nPlease allow this app in System Settings > Privacy & Security > Accessibility", "アクセシビリティの権限がありません。リモート入力ができません\nシステム設定 > プライバシーとセキュリティ > アクセシビリティ で許可してください", "손쉬운 사용 권한이 없어 원격 입력이 불가능합니다\n시스템 설정 > 개인정보 및 보안 > 손쉬운 사용에서 허용하세요", "Keine Bedienungshilfen-Berechtigung; Remote-Eingabe nicht möglich\nBitte in Systemeinstellungen > Datenschutz & Sicherheit > Bedienungshilfen erlauben", "Autorisation d'accessibilité désactivée; saisie à distance impossible\nAutorisez l'app dans Réglages Système > Confidentialité et sécurité > Accessibilité", "Нет разрешения на специальные возможности, удаленный ввод недоступен\nРазрешите приложению доступ в Системные настройки > Конфиденциальность и безопасность > Специальные возможности"),

    // ---- Ask window ----
    E("ask.title", "连接请求", "連接請求", "Connection request", "接続リクエスト", "연결 요청", "Verbindungsanfrage", "Demande de connexion", "Запрос подключения"),
    E("ask.text", "%s(%s) 请求%s本机", "%s(%s) 請求%s本機", "%s(%s) wants to %s this device", "%s(%s) が本機を%sしています", "%s(%s) 이 기기를 %s하려 함", "%s(%s) möchte dieses Gerät %s", "%s(%s) veut %s cet appareil", "%s(%s) хочет %s это устройство"),
    E("ask.accept", "接受", "接受", "Accept", "承認", "수락", "Akzeptieren", "Accepter", "Принять"),
    E("ask.reject", "拒绝", "拒絕", "Reject", "拒否", "거부", "Ablehnen", "Refuser", "Отклонить"),

    // ---- File transfer ----
    E("file.title", "发送文件", "發送文件", "Send Files", "ファイル送信", "파일 보내기", "Dateien senden", "Envoyer des fichiers", "Отправка файлов"),
    E("file.addFiles", "选择文件", "選擇文件", "Add files", "ファイルを選択", "파일 선택", "Dateien wählen", "Ajouter des fichiers", "Выбрать файлы"),
    E("file.send", "发送", "發送", "Send", "送信", "보내기", "Senden", "Envoyer", "Отправить"),
    E("file.cancel", "取消", "取消", "Cancel", "キャンセル", "취소", "Abbrechen", "Annuler", "Отмена"),
    E("filebrowser.title", "文件传输", "文件傳輸", "File Transfer", "ファイル転送", "파일 전송", "Dateiübertragung", "Transfert de fichiers", "Передача файлов"),
    E("filebrowser.local", "本机", "本機", "Local", "ローカル", "로컬", "Lokal", "Local", "Локально"),
    E("filebrowser.remote", "远程", "遠端", "Remote", "リモート", "원격", "Remote", "Distant", "Удалённо"),
    E("filebrowser.up", "上级目录", "上級目錄", "Up", "上へ", "위로", "Hoch", "Monter", "Вверх"),
    E("filebrowser.upload", "点击上传到远程", "點擊上傳到遠端", "Click to upload to remote", "クリックでアップロード", "클릭하여 원격에 업로드", "Klicken zum Hochladen", "Cliquer pour envoyer", "Нажмите, чтобы загрузить"),
    E("filebrowser.download", "点击下载到本机", "點擊下載到本機", "Click to download to local", "クリックでダウンロード", "클릭하여 로컬에 다운로드", "Klicken zum Herunterladen", "Cliquer pour télécharger", "Нажмите, чтобы скачать"),
    E("filebrowser.empty", "空目录", "空目錄", "Empty", "空", "비어 있음", "Leer", "Vide", "Пусто"),
    E("filebrowser.uploading", "正在上传...", "正在上傳...", "Uploading...", "アップロード中...", "업로드 중...", "Lade hoch...", "Envoi...", "Загрузка..."),
    E("filebrowser.downloading", "正在下载...", "正在下載...", "Downloading...", "ダウンロード中...", "다운로드 중...", "Lade herunter...", "Téléchargement...", "Скачивание..."),
    E("filebrowser.enter", "进入文件夹", "進入文件夾", "Open folder", "フォルダを開く", "폴더 열기", "Ordner öffnen", "Ouvrir le dossier", "Открыть папку"),
    E("filebrowser.refresh", "刷新", "刷新", "Refresh", "更新", "새로고침", "Aktualisieren", "Actualiser", "Обновить"),
    E("file.dragHint", "将文件拖到此处", "將文件拖到此處", "Drop files here", "ここにファイルをドロップ", "파일을 여기에 놓으세요", "Dateien hierher ziehen", "Déposez les fichiers ici", "Перетащите файлы сюда"),
    E("file.name", "文件名", "文件名", "Name", "ファイル名", "파일 이름", "Name", "Nom", "Имя"),
    E("file.size", "大小", "大小", "Size", "サイズ", "크기", "Größe", "Taille", "Размер"),
    E("file.progress", "进度", "進度", "Progress", "進捗", "진행", "Fortschritt", "Progression", "Прогресс"),
    E("file.status", "状态", "狀態", "Status", "状態", "상태", "Status", "État", "Статус"),
    E("file.waiting", "等待对方接受…", "等待對方接受…", "Waiting for acceptance…", "相手の承認待ち…", "상대 수락 대기 중…", "Warte auf Bestätigung…", "En attente d'acceptation…", "Ожидание подтверждения…"),
    E("file.transferring", "传输中", "傳輸中", "Transferring", "転送中", "전송 중", "Übertrage", "Transfert", "Передача"),
    E("file.done", "完成", "完成", "Done", "完了", "완료", "Fertig", "Terminé", "Готово"),
    E("file.failed", "失败", "失敗", "Failed", "失敗", "실패", "Fehlgeschlagen", "Échec", "Ошибка"),
    E("file.rejected", "对方拒绝", "對方拒絕", "Rejected", "拒否されました", "거부됨", "Abgelehnt", "Refusé", "Отклонено"),
    E("file.cancelled", "已取消", "已取消", "Cancelled", "キャンセル済み", "취소됨", "Abgebrochen", "Annulé", "Отменено"),
    E("file.incoming", "收到文件请求", "收到文件請求", "Incoming file", "ファイル受信", "수신 파일", "Eingehende Datei", "Fichier entrant", "Входящий файл"),
    E("file.accept", "接受", "接受", "Accept", "受信", "수락", "Annehmen", "Accepter", "Принять"),
    E("file.reject", "拒绝", "拒絕", "Reject", "拒否", "거부", "Ablehnen", "Refuser", "Отклонить"),
    E("file.receiveDir", "接收目录", "接收目錄", "Receive folder", "受信フォルダ", "받는 폴더", "Empfangsordner", "Dossier de réception", "Папка приема"),
    E("file.openDir", "打开目录", "打開目錄", "Open folder", "フォルダを開く", "폴더 열기", "Ordner öffnen", "Ouvrir le dossier", "Открыть папку"),
    E("file.empty", "暂无传输记录", "暫無傳輸記錄", "No transfers yet", "転送履歴はありません", "전송 기록 없음", "Noch keine Übertragungen", "Aucun transfert", "Передач пока нет"),
    E("file.pathInvalid", "文件不存在或无法读取", "文件不存在或無法讀取", "File does not exist or cannot be read", "ファイルが存在しないか読み込めません", "파일이 없거나 읽을 수 없음", "Datei existiert nicht oder kann nicht gelesen werden", "Fichier inexistant ou illisible", "Файл не существует или не читается"),
    E("file.saveTo", "保存到", "保存到", "Saved to", "保存先", "저장 위치", "Gespeichert in", "Enregistré dans", "Сохранено в"),
    E("file.busy", "正在处理其他传输,请稍候", "正在處理其他傳輸,請稍候", "Another transfer in progress, please wait", "他の転送を処理中です。お待ちください", "다른 전송 처리 중, 잠시 기다리세요", "Andere Übertragung läuft, bitte warten", "Autre transfert en cours, veuillez patienter", "Идет другая передача, подождите"),

    // ---- Settings ----
    E("settings.title", "连接设置", "連接設置", "Connection Settings", "接続設定", "연결 설정", "Verbindungseinstellungen", "Paramètres de connexion", "Настройки подключения"),
    E("settings.theme", "主题", "主題", "Theme", "テーマ", "테마", "Design", "Thème", "Тема"),
    E("settings.dark", "暗色", "暗色", "Dark", "ダーク", "어두운", "Dunkel", "Sombre", "Темная"),
    E("settings.light", "亮色", "亮色", "Light", "ライト", "밝은", "Hell", "Clair", "Светлая"),
    E("settings.language", "语言", "語言", "Language", "言語", "언어", "Sprache", "Langue", "Язык"),
    E("settings.openSettings", "设置", "設置", "Settings", "設定", "설정", "Einstellungen", "Paramètres", "Настройки"),
};

#undef E

} // namespace

Lang Language::s_lang = Lang::ZhCN;

void Language::Set(Lang lang)
{
    if ((int)lang >= 0 && (int)lang < (int)Lang::Count) {
        s_lang = lang;
    }
}

std::string Language::NativeName(Lang lang)
{
    switch (lang) {
    case Lang::ZhCN: return ui::StringConvert::UTF8ToT("简体中文");
    case Lang::ZhTW: return ui::StringConvert::UTF8ToT("繁體中文");
    case Lang::En:   return ui::StringConvert::UTF8ToT("English");
    case Lang::Ja:   return ui::StringConvert::UTF8ToT("日本語");
    case Lang::Ko:   return ui::StringConvert::UTF8ToT("한국어");
    case Lang::De:   return ui::StringConvert::UTF8ToT("Deutsch");
    case Lang::Fr:   return ui::StringConvert::UTF8ToT("Français");
    case Lang::Ru:   return ui::StringConvert::UTF8ToT("Русский");
    default:         return ui::StringConvert::UTF8ToT("English");
    }
}

const char* Language::Code(Lang lang)
{
    switch (lang) {
    case Lang::ZhCN: return "zh_CN";
    case Lang::ZhTW: return "zh_TW";
    case Lang::En:   return "en_US";
    case Lang::Ja:   return "ja_JP";
    case Lang::Ko:   return "ko_KR";
    case Lang::De:   return "de_DE";
    case Lang::Fr:   return "fr_FR";
    case Lang::Ru:   return "ru_RU";
    default:         return "en_US";
    }
}

std::string Language::Tr(const char* key)
{
    const int idx = (int)s_lang;
    for (const Entry& e : kEntries) {
        if (std::strcmp(e.key, key) != 0) {
            continue;
        }
        const char* const cols[8] = { e.zhCN, e.zhTW, e.en, e.ja, e.ko, e.de, e.fr, e.ru };
        const char* s = cols[idx];
        if (s == nullptr || s[0] == '\0') {
            s = e.en; // fall back to English
        }
        return ui::StringConvert::UTF8ToT(s);
    }
    return ui::StringConvert::UTF8ToT(key); // keep the key as last resort
}

std::string Language::TrUtf8(const char* key)
{
    return ui::StringConvert::TToUTF8(Tr(key));
}

} // namespace sdk
