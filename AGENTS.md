# Quy tắc dự án ChatAI TPIOS ESP32-S3

Các quy tắc trong tệp này là bắt buộc đối với mọi lần làm việc, chỉnh sửa và hỗ trợ dự án.

## 1. Bảo vệ giao diện Web Admin — QUY TẮC BẮT BUỘC

- Giao diện Web Admin hiện có trên branch `main` là **baseline được người dùng chấp thuận**.
- Không tự ý xóa, thay thế, thiết kế lại hoặc thay đổi giao diện.
- Không tự ý sửa HTML, CSS, JavaScript, bố cục, màu sắc, kích thước, thanh điều hướng, tên tab, nội dung hiển thị, nút bấm hoặc cách tương tác.
- Không được xóa hay làm thay đổi chức năng hiện có của Web Admin.
- Mọi thay đổi dù nhỏ liên quan đến giao diện đều phải được trình bày trước: file/khu vực bị tác động, nội dung thay đổi, lý do và ảnh hưởng dự kiến. **Chỉ thực hiện sau khi người dùng xác nhận rõ ràng.**
- Nếu sửa firmware hoặc API phía sau, phải duy trì tương thích với giao diện hiện tại. Không được âm thầm đổi tên endpoint, schema JSON, phương thức HTTP, luồng lưu hoặc hành vi mà giao diện đang sử dụng.
- Nếu phát hiện lỗi giao diện, chỉ báo cáo và đề xuất cách sửa; không tự sửa trước khi được phép.
- Không coi yêu cầu sửa chức năng backend là sự cho phép sửa giao diện frontend.

## 2. Quy trình trước khi chỉnh sửa

1. Đọc quy tắc này trước khi bắt đầu công việc.
2. Kiểm tra trạng thái repository, branch hiện tại, HEAD và cấu trúc tệp trên GitHub.
3. Đọc nội dung thực tế của các tệp liên quan ở branch `main`; không suy đoán dựa trên bản cũ, log cũ hoặc tên hàm.
4. Đánh giá ảnh hưởng đến các chức năng hiện hữu và Web Admin baseline.
5. Ưu tiên thay đổi nhỏ nhất có thể; không thêm thư viện hoặc abstraction không cần thiết.
6. Nếu thay đổi có thể ảnh hưởng giao diện, dữ liệu cấu hình, kết nối, OTA, âm thanh hoặc bảo mật, phải giải thích rủi ro và xin xác nhận khi chưa có sự cho phép rõ ràng.
7. Sau khi sửa, kiểm tra diff/source và báo chính xác những gì đã thay đổi. Không tuyên bố đã build hoặc kiểm thử phần cứng nếu chưa thực hiện.

## 3. Git và quyền thực hiện

- Chỉ làm việc trên branch `main`.
- Không tạo branch mới, không chuyển sang branch khác.
- Chỉ sửa đúng phần đang lỗi, không được những phần đang chạy ok.
- Không tự chạy GitHub Actions, workflow, Codemagic, build, flash hoặc OTA nếu chưa được người dùng cho phép riêng.
- Không tự ý reset, force-push, xóa tệp hoặc ghi đè thay đổi của người dùng.

## 4. Bảo toàn chức năng firmware

Mọi thay đổi phải xem xét và bảo toàn các phần đang hoạt động, đặc biệt:
- Quy trình Wi-Fi SoftAP/STA provisioning, phản hồi sai mật khẩu, DHCP, DNS và kiểm tra Internet.
- Web Admin và khả năng truy cập OTA qua SoftAP khi thiết bị chưa có Internet.
- Gemini Live, WebSocket/TLS và xác minh chứng chỉ.
- API Key: không ghi log, không trả lại đầy đủ cho trình duyệt, không đưa vào mã nguồn hoặc Git.
- System Prompt và các cấu hình được lưu bền vững trong NVS hoặc vùng lưu trữ phù hợp.
- I2S, microphone, speaker, audio pipeline, nút bấm và OLED.
- OTA không được tự xóa Wi-Fi, API Key hoặc cấu hình người dùng.

Không thay đổi một pipeline hoặc giao thức chỉ để làm cho một phần khác thuận tiện hơn nếu chưa đánh giá tác động và được người dùng chấp thuận khi cần.

## 5. Báo cáo và xác minh

- Phân biệt rõ: đã sửa source, đã kiểm tra source-level, đã build thành công, đã chạy trên thiết bị.
- Không khẳng định compile/runtime thành công khi chưa có bằng chứng.
- Khi chưa build hoặc chưa flash, phải nói rõ.
- Báo commit SHA và liên kết commit sau khi thao tác commit thành công.
- Nếu thiếu thông tin hoặc phát hiện yêu cầu có thể phá vỡ baseline, dừng lại và hỏi người dùng thay vì tự quyết định.

## 6. Ưu tiên khi có xung đột

Yêu cầu và xác nhận mới nhất của người dùng được ưu tiên, nhưng không được diễn giải một yêu cầu chung là quyền sửa Web Admin. Với mọi thay đổi giao diện, phải có sự đồng ý rõ ràng, cụ thể trước khi thực hiện.


## 7. ECC Shin — quy trình điều phối dự án

- Bộ quy tắc ECC Shin của repository nằm trong `ECC-SHIN/`; bắt đầu từ `ECC-SHIN/CORE/ECC-SHIN-CORE.md`.
- Trước mỗi nhiệm vụ, đọc ECC Core và 5 tài liệu trong `ECC-SHIN/PROJECT/` có liên quan.
- Chọn vai trò chuyên gia theo `ECC-SHIN/CORE/AGENT-ROLES.md`; đây là các vai trò chuyên môn được điều phối, không phải 12 AI độc lập.
- Chọn quy trình trong `ECC-SHIN/CORE/SKILL-CATALOG.md` và các hướng dẫn tại `ECC-SHIN/SKILLS/`.
- Phân loại bằng chứng bằng VERIFIED, USER-REPORTED, ASSUMED, BLOCKED và NOT TESTED; không biến giả thuyết thành sự thật.
- Chế độ ASK: phải xin phép trước mỗi thao tác sửa/tạo/xóa file, commit/push, thay đổi cấu hình hoặc dữ liệu. Sự chấp thuận chỉ áp dụng đúng phạm vi được nêu.
- Build, GitHub Actions, Codemagic, flash, OTA và deploy luôn cần sự cho phép riêng, kể cả khi đã được duyệt sửa source.
- ECC Shin bổ sung quy trình, không thay thế hoặc làm suy yếu các quy tắc bảo vệ Web Admin, firmware, secrets và branch ở các mục trên.
