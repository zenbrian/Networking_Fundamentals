import os
from PIL import Image, ImageDraw, ImageFont

def create_annotated_image():
    input_path = "docs/images/Day14/terminal2.png"
    output_path = "docs/images/Day14/terminal2_annotated.png"

    if not os.path.exists(input_path):
        print(f"File not found: {input_path}")
        return

    term_img = Image.open(input_path).convert("RGBA")
    term_w, term_h = term_img.size

    # Canvas dimensions
    canvas_pad = 30
    header_h = 75
    right_w = 640
    total_w = canvas_pad + term_w + 30 + right_w + canvas_pad
    total_h = canvas_pad + header_h + term_h + canvas_pad

    # Create background canvas
    canvas = Image.new("RGBA", (total_w, total_h), (15, 23, 42, 255)) # slate-900
    draw = ImageDraw.Draw(canvas)

    # Load fonts
    font_bold_path = "C:/Windows/Fonts/msjhbd.ttc"
    font_reg_path = "C:/Windows/Fonts/msjh.ttc"

    title_font = ImageFont.truetype(font_bold_path, 26)
    subtitle_font = ImageFont.truetype(font_reg_path, 15)
    card_title_font = ImageFont.truetype(font_bold_path, 17)
    card_text_font = ImageFont.truetype(font_reg_path, 13)
    card_bold_font = ImageFont.truetype(font_bold_path, 13)

    # Draw Header
    draw.text((canvas_pad, canvas_pad), "Day 14：UDP 收發完整生命週期與底層解析", font=title_font, fill=(56, 189, 248, 255))
    draw.text((canvas_pad, canvas_pad + 38), "從 ARP 廣播問路、IPv4 本機簽收、UDP 應用層執行到主動封裝發送的完整鏈路", font=subtitle_font, fill=(148, 163, 184, 255))

    # Paste Terminal Image with border
    term_x = canvas_pad
    term_y = canvas_pad + header_h
    
    # Outer terminal glow/border
    draw.rectangle([term_x - 3, term_y - 3, term_x + term_w + 2, term_y + term_h + 2], outline=(51, 65, 85, 255), width=2)
    canvas.paste(term_img, (term_x, term_y))

    # Define annotations on the right
    cards = [
        {
            "tag": "1. ARP 廣播問路與地址學習",
            "tag_color": (59, 130, 246, 255), # blue
            "bg_color": (30, 41, 59, 230),
            "border_color": (59, 130, 246, 180),
            "term_y_target": term_y + 60,
            "y": term_y + 10,
            "h": 145,
            "lines": [
                ("• 廣播目的：", "Linux (Terminal 3) 發送 ff:ff:ff:ff:ff:ff 廣播，尋找 10.0.0.2 的 MAC。"),
                ("• 本機回覆：", "Terminal 2 回覆 ARP Reply，宣告本機 MAC 為 02:00:00:00:00:01。"),
                ("• 暗中關鍵：", "在回覆的同時，堆疊自動將 10.0.0.1 -> c6:bf:60:33:9d:73 存入 ARP Table！"),
                ("  ", "這正是後續 udp_send 能查到對方真實 MAC 並順利送達的前提。")
            ]
        },
        {
            "tag": "2. IPv4 單播抵達與本機簽收 (Local Delivery)",
            "tag_color": (16, 185, 129, 255), # emerald
            "bg_color": (30, 41, 59, 230),
            "border_color": (16, 185, 129, 180),
            "term_y_target": term_y + 240,
            "y": term_y + 175,
            "h": 145,
            "lines": [
                ("• 單播傳輸：", "Linux 取得 MAC 後，以單播 (02:00:00:00:00:01) 正式發送 IPv4 封包。"),
                ("• 本機判定：", "目的 IP 為 10.0.0.2（恰好等於本機 IP），直接判定為 Local Delivery 簽收。"),
                ("• 為什麼沒看到遮罩？", "IPv4 封包標頭本來就沒有 Subnet Mask 欄位！"),
                ("  ", "子網路遮罩只存在於主機端路由表。目的 IP 正是自己時，無需查表比對遮罩。")
            ]
        },
        {
            "tag": "3. UDP 埠號分流與應用層回呼 (Callback)",
            "tag_color": (245, 158, 11, 255), # amber
            "bg_color": (30, 41, 59, 230),
            "border_color": (245, 158, 11, 180),
            "term_y_target": term_y + 450,
            "y": term_y + 340,
            "h": 145,
            "lines": [
                ("• 埠號對應：", "Source Port 44565 (Linux 隨機臨時 Port) -> Destination Port 8080。"),
                ("• Socket 查表：", "查詢 Socket Table 命中 8080，提取 Payload: \"trigger\\n\" (8 bytes)。"),
                ("• 應用程式執行：", "觸發 udp_echo_app Callback 函式，將資料輸出在 Terminal 2 螢幕上！"),
                ("  ", "(註：printf 是輸出在本機 Terminal 2 終端機，而非客戶端 Terminal 3)")
            ]
        },
        {
            "tag": "4. 反手發送：udp_send 主動封裝出擊！",
            "tag_color": (168, 85, 247, 255), # purple
            "bg_color": (30, 41, 59, 230),
            "border_color": (168, 85, 247, 180),
            "term_y_target": term_y + 640,
            "y": term_y + 505,
            "h": 160,
            "lines": [
                ("• 主動出擊：", "udp_echo_app 呼叫 udp_send，啟動逆向封裝 (Encapsulation)。"),
                ("• 查表組裝：", "查 ARP 表取得 c6:bf:60:33:9d:73，由內向外組裝 Payload -> UDP -> IP -> Eth。"),
                ("• 52 Bytes 驗算：", "Eth(14) + IP(20) + UDP(8) + \"Hello UDP\\n\"(10) = 52 Bytes！完全精準！"),
                ("• 接收驗證：", "封包經 tap0 注入 Linux 核心，Terminal 1 (nc -lu 9999) 成功印出 Hello UDP！")
            ]
        }
    ]

    right_x = term_x + term_w + 35

    for card in cards:
        cx = right_x
        cy = card["y"]
        cw = right_w
        ch = card["h"]

        # Draw connecting line from terminal to card
        target_y = card["term_y_target"]
        line_start = (term_x + term_w, target_y)
        line_mid = (cx - 15, cy + 25)
        line_end = (cx, cy + 25)

        draw.line([line_start, line_mid, line_end], fill=card["tag_color"], width=2)
        draw.ellipse([line_start[0] - 4, line_start[1] - 4, line_start[0] + 4, line_start[1] + 4], fill=card["tag_color"])

        # Draw card background
        draw.rounded_rectangle([cx, cy, cx + cw, cy + ch], radius=8, fill=card["bg_color"], outline=card["border_color"], width=2)

        # Draw badge/title
        draw.rounded_rectangle([cx + 12, cy + 10, cx + 12 + 10, cy + 28], radius=3, fill=card["tag_color"])
        draw.text((cx + 28, cy + 9), card["tag"], font=card_title_font, fill=(248, 250, 252, 255))

        # Draw content lines
        line_y = cy + 40
        for prefix, text in card["lines"]:
            draw.text((cx + 16, line_y), prefix, font=card_bold_font, fill=card["tag_color"])
            prefix_w = draw.textlength(prefix, font=card_bold_font)
            draw.text((cx + 16 + prefix_w + 4, line_y), text, font=card_text_font, fill=(226, 232, 240, 255))
            line_y += 24

    # Save image
    canvas.save(output_path, "PNG")
    print(f"Annotated image successfully created: {output_path}")

if __name__ == "__main__":
    create_annotated_image()
