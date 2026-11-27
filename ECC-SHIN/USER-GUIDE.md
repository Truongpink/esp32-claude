# ECC Shin — Hướng dẫn sử dụng

Tài liệu này hướng dẫn người dùng giao nhiệm vụ cho ECC Shin trong dự án ChatAI TPIOS ESP32-S3. Có thể sử dụng toàn bộ câu lệnh bằng tiếng Việt tự nhiên; không cần nhớ tên file, agent hay skill.

## 1. ECC Shin là gì?

ECC Shin là bộ quy tắc và quy trình kỹ thuật được tích hợp trong repository. ECC Core đọc yêu cầu, chọn các vai trò chuyên môn phù hợp, áp dụng Skills liên quan, kiểm tra bằng chứng và điều phối quy trình.

Các vai trò chuyên gia là những góc nhìn chuyên môn do ECC Core điều phối, không phải 12 AI độc lập hoặc dịch vụ tự chạy nền.

ECC Shin hỗ trợ:
- Quản lý và lập kế hoạch dự án.
- Phân tích kiến trúc và quyết định kỹ thuật.
- Phát triển, gỡ lỗi và kiểm thử.
- Rà soát bảo mật, chất lượng và hiệu năng.
- Nghiên cứu và cập nhật tài liệu.

## 2. Cách bắt đầu nhanh

Chỉ cần mô tả mục tiêu, hiện tượng, kết quả mong muốn và giới hạn quan trọng. Ví dụ:

> ECC Shin, kiểm tra vì sao Chat Voice đôi khi không phát được âm thanh. Hãy đọc source trên main, tìm nguyên nhân có bằng chứng, chưa sửa code và báo cáo hướng xử lý.

Không bắt buộc phải viết tiền tố “ECC Shin”; có thể giao việc bằng tiếng Việt bình thường. Khi muốn nhấn mạnh quy trình, hãy dùng tiền tố này.

## 3. Các vai trò chuyên gia

| Vai trò | Dùng khi cần |
|---|---|
| Project Manager | Theo dõi phạm vi, tiến độ, rủi ro và bàn giao |
| Planner | Chia yêu cầu thành các bước triển khai |
| System Architect | Phân tích kiến trúc, luồng dữ liệu và tương thích |
| Senior Developer | Thiết kế và thực hiện thay đổi mã nguồn đã duyệt |
| Debugger | Truy tìm nguyên nhân lỗi từ source, log và bằng chứng |
| Tester | Xây dựng test plan và đánh giá kết quả kiểm thử |
| Security Engineer | Kiểm tra secrets, xác thực, mạng và bề mặt tấn công |
| Code Reviewer | Rà soát diff, lỗi logic và tác động ngoài ý muốn |
| Build Engineer | Phân tích cấu hình và quy trình build; chỉ chạy khi được phép |
| Documentation Engineer | Cập nhật tài liệu và project memory khi được duyệt |
| Performance Engineer | Phân tích RAM, CPU, độ trễ và tài nguyên |
| Research Engineer | Nghiên cứu giao thức, thư viện và tài liệu kỹ thuật |

Anh không cần tự chọn vai trò. ECC Core sẽ chọn một hoặc nhiều vai trò phù hợp. Có thể yêu cầu rõ một vai trò, ví dụ: “Dùng Debugger và Security Engineer để rà soát phần đăng nhập Web Admin”.

## 4. Câu lệnh mẫu

### Kiểm tra tổng thể dự án — chỉ đọc

```text
ECC Shin: Kiểm tra tổng thể repository trên branch main.
Đánh giá kiến trúc, trạng thái các tính năng, rủi ro và
những vấn đề cần ưu tiên. Chỉ đọc, không sửa file,
không commit, không chạy build hoặc Actions.
```

### Điều tra một lỗi

```text
ECC Shin: Điều tra lỗi [mô tả lỗi].
Đọc source thực tế trên main, lần theo luồng gọi,
đối chiếu log nếu có, phân biệt nguyên nhân đã xác minh
với giả thuyết. Đề xuất thay đổi tối thiểu và chờ tôi duyệt.
```

### Sửa lỗi theo nguyên nhân gốc

```text
ECC Shin: Điều tra và xử lý lỗi [mô tả lỗi].
Kiểm tra source, luồng chạy và log thực tế trên main.
Nếu đã đủ bằng chứng xác định nguyên nhân, hãy đề xuất
sửa đúng nguyên nhân và thêm log xác minh có mục tiêu
trong cùng đợt thay đổi. Không chỉ thêm log rồi chờ tôi
chạy lại nếu đã có đủ cơ sở để sửa.
Nếu chưa đủ bằng chứng, chỉ đề xuất phép kiểm tra/log
cụ thể để phân biệt các giả thuyết còn lại.
Nêu rõ file, rủi ro và kế hoạch kiểm tra; chờ tôi duyệt
trước khi sửa. Không tự chạy build/Actions/flash/OTA.
```

### Lập kế hoạch tính năng mới

```text
ECC Shin: Lập kế hoạch triển khai [tên tính năng].
Phân tích kiến trúc hiện tại, các thành phần bị ảnh hưởng,
tương thích ngược, rủi ro và phương án kiểm thử.
Liệt kê chính xác các file dự kiến thay đổi.
Chưa chỉnh sửa cho đến khi tôi phê duyệt.
```

### Thực hiện thay đổi đã duyệt

```text
ECC Shin: Thực hiện đúng phương án đã được tôi duyệt.
Trước khi sửa, kiểm tra lại branch, HEAD, trạng thái repo
và nội dung source liên quan. Chỉ thay đổi các file
trong phạm vi đã duyệt; không mở rộng phạm vi.
Sau đó rà soát diff và báo kết quả.
```

### Rà soát bảo mật

```text
ECC Shin: Dùng Security Engineer và Code Reviewer
rà soát [khu vực/tính năng]. Tập trung vào secrets,
xác thực, quyền truy cập, dữ liệu nhạy cảm và log.
Chỉ báo cáo, không sửa.
```

### Kiểm tra trước khi bàn giao

```text
ECC Shin: Rà soát source và diff của thay đổi hiện tại.
Kiểm tra lỗi logic, hồi quy, bảo mật và tài liệu.
Nêu rõ phần nào đã kiểm tra, phần nào chưa test.
Không tự chạy build, Actions, flash hoặc OTA.
```

## 5. Quy trình duyệt và quyền thao tác

ECC Shin áp dụng chế độ ASK. Mặc định, việc phân tích và đọc repository được thực hiện trong phạm vi quyền truy cập đã cấp; mọi thay đổi cần được duyệt đúng phạm vi.

| Thao tác | Quy tắc |
|---|---|
| Đọc source, cấu trúc, tài liệu | Được phép trong phạm vi truy cập |
| Tra cứu tài liệu công khai | Được phép |
| Tạo/sửa/xóa file | Phải xin phép trước |
| Commit trực tiếp lên `main` | Đã được cấp quyền thường trực sau khi nội dung thay đổi được anh duyệt; không cần hỏi lại riêng quyền commit |
| Tạo branch | Không tạo; dự án chỉ làm trên `main` |
| GitHub Actions, CI, Codemagic, build | Cần cho phép riêng |
| Flash, OTA, deploy | Cần cho phép riêng |
| Thay đổi secrets/quyền truy cập | Phải xin phép và xác định rõ phạm vi |

Một lần duyệt chỉ áp dụng cho các file và phạm vi thay đổi được nêu. Sau khi nội dung thay đổi được duyệt, trợ lý được tự commit trực tiếp lên `main` mà không cần hỏi lại riêng bước commit. Quyền này không bao gồm tạo branch, force-push, reset lịch sử, build, chạy Actions, flash, OTA hay deploy. Không được suy diễn quyền sửa nội dung từ câu “ok” nếu chưa rõ nó đang duyệt việc gì.

Khi duyệt một danh sách thay đổi, nên nêu rõ:
- File được phép tạo hoặc sửa.
- Nội dung/phạm vi thay đổi.
- Các giới hạn thao tác khác nếu có; quyền commit lên `main` đã được cấp thường trực và không cần duyệt lại.
- Các thao tác bị cấm hoặc chưa được duyệt.

## 6. Quy tắc riêng của ChatAI TPIOS

- Chỉ làm việc trên branch `main`; không tạo branch mới.
- Trước mọi chỉnh sửa, kiểm tra branch, HEAD, cấu trúc và source thực tế.
- Ưu tiên thay đổi nhỏ nhất; không thêm thư viện nếu không cần.
- Bảo toàn các chức năng đang hoạt động và baseline Web Admin.
- Không sửa giao diện Web Admin nếu chưa được duyệt cụ thể.
- Không để lộ API key, mật khẩu, token hoặc secrets trong source, log, OLED hay Git.
- Không tuyên bố build, runtime hoặc phần cứng đã kiểm thử nếu không có bằng chứng.
- Phân biệt rõ source-level verification, build verification và hardware verification.

### Thứ tự ưu tiên voice backend

Hiện tại dự án theo định hướng **Xiaozhi-first, Gemini-later**:

1. Hoàn thiện và ổn định Chat Voice sử dụng Xiaozhi.
2. Kiểm tra microphone, speaker, hội thoại, nút BOOT/PTT và các thông báo liên quan.
3. Sửa lỗi và xác minh ổn định theo từng bước.
4. Chỉ khi người dùng xác nhận Xiaozhi đã ổn định mới lập kế hoạch chuyển sang Gemini Live.
5. Không tự bật Gemini, xóa code Gemini hoặc thay backend khi chưa được duyệt.

Gemini Live đang tạm đóng có chủ đích trong giai đoạn ưu tiên Xiaozhi; không tự coi trạng thái này là lỗi cần sửa.

## 7. Bằng chứng và báo cáo

ECC Shin phải gắn nhãn thông tin quan trọng:

- **VERIFIED** — đã xác minh trực tiếp bằng source, cấu hình hoặc kết quả kiểm tra.
- **USER-REPORTED** — do người dùng cung cấp, chưa tự xác minh.
- **ASSUMED** — giả định cần kiểm chứng.
- **BLOCKED** — chưa thể tiếp tục vì thiếu quyền, dữ liệu hoặc điều kiện.
- **NOT TESTED** — chưa chạy kiểm thử tương ứng.

Báo cáo nên có:
1. Kết luận ngắn gọn.
2. Bằng chứng và file/vị trí liên quan.
3. Nguyên nhân đã xác minh và giả thuyết (tách riêng).
4. Tác động/rủi ro.
5. Phương án tối thiểu.
6. Những gì cần người dùng phê duyệt.
7. Trạng thái kiểm thử và giới hạn chưa xác minh.

## 8. Tiếp tục công việc ở cuộc trò chuyện mới

Khi mở cuộc trò chuyện mới, anh có thể gửi:

```text
Tiếp tục dự án ChatAI TPIOS ESP32-S3.
Hãy đọc AGENTS.md, ECC-SHIN/CORE/ECC-SHIN-CORE.md
và các Project Memory liên quan trên branch main.
Kiểm tra trạng thái repository hiện tại trước khi làm.
Tiếp tục theo các quyết định kỹ thuật mới nhất;
không suy đoán trạng thái từ cuộc trò chuyện cũ.
```

Nếu đang tiếp tục một lỗi hoặc tính năng cụ thể, hãy kèm tên tính năng, log mới nhất hoặc kết quả kiểm tra gần nhất.

## 9. Các tài liệu ECC Shin liên quan

- Core: `ECC-SHIN/CORE/ECC-SHIN-CORE.md`
- Vai trò: `ECC-SHIN/CORE/AGENT-ROLES.md`
- Danh mục Skills: `ECC-SHIN/CORE/SKILL-CATALOG.md`
- Chính sách bảo mật: `ECC-SHIN/CORE/SECURITY-POLICY.md`
- Project Memory: `ECC-SHIN/PROJECT/`
- Skills: `ECC-SHIN/SKILLS/`

**Nguyên tắc dễ nhớ:** Anh mô tả mục tiêu và giới hạn; ECC Shin phân tích, chọn chuyên gia, đưa ra phương án và xin duyệt trước khi thay đổi.