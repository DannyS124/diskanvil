# The text kinds for the tests (see make.sh): an email and a mailbox made by Python's own email
# and mailbox modules, and a calendar, contacts, an SVG drawing and an XML file written to their
# specifications.
#   python3 -I texts.py <folder>
import datetime
import email.message
import email.policy
import email.utils
import mailbox
import os
import sys

out = sys.argv[1]
when = datetime.datetime(2024, 7, 14, 13, 22, 5, tzinfo=datetime.timezone.utc)

m = email.message.EmailMessage(policy=email.policy.SMTP)
m["From"] = email.utils.formataddr(("Sam Tëster", "sam@example.com"))
m["To"] = "Alex Example <alex@example.com>"
m["Subject"] = "Fähre: the tickets for Sunday"
m["Date"] = email.utils.format_datetime(when)
m["Message-ID"] = "<ferry-1@example.com>"
m.set_content("Hi Alex,\n\nThe ferry tickets are attached. Café at pier 3 at 07:30?\n\n" + "See you there. " * 40 + "\n\nSam\n",
              cte="quoted-printable")
m.add_alternative("<html><body><p>Hi Alex,</p><p>The <b>ferry</b> tickets are attached.</p></body></html>", subtype="html")
m.add_attachment(b"%PDF-1.4\n% a made-up ticket\n", maintype="application", subtype="pdf", filename="Fähre tickets.pdf")
with open(f"{out}/ferry.eml", "wb") as f:
    f.write(m.as_bytes())

path = f"{out}/Inbox.mbox"
if os.path.exists(path):
    os.remove(path)
box = mailbox.mbox(path)
for i, subject in enumerate(["Lunch on Friday?", "Photos from the lake", "Re: Photos from the lake"]):
    msg = email.message.EmailMessage()
    msg["From"] = "Sam Tester <sam@example.com>"
    msg["To"] = "alex@example.com"
    msg["Subject"] = subject
    msg["Date"] = email.utils.format_datetime(when + datetime.timedelta(days=i))
    msg.set_content(f"Message {i + 1}.\n")
    box.add(msg)
box.flush()
box.close()

with open(f"{out}/trip.ics", "w", newline="") as f:
    f.write("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//DiskForge tests//EN\r\nX-WR-CALNAME:Trips\r\n"
            "BEGIN:VEVENT\r\nUID:1@example.com\r\nDTSTAMP:20240701T090000Z\r\nDTSTART:20240714T053000Z\r\n"
            "SUMMARY:Ferry to the island\r\nLOCATION:Pier 3\r\nEND:VEVENT\r\n"
            "BEGIN:VEVENT\r\nUID:2@example.com\r\nDTSTAMP:20240701T090000Z\r\nDTSTART;VALUE=DATE:20240716\r\n"
            "SUMMARY:Walk up the mountain\\, if it's dry\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n")

with open(f"{out}/phone.vcf", "w", newline="") as f:
    f.write("BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alex Example\r\nN:Example;Alex;;;\r\nTEL;TYPE=CELL:+1 555 0100\r\n"
            "EMAIL:alex@example.com\r\nEND:VCARD\r\n"
            # vCard 2.1, as older phones write them: the name in quoted-printable UTF-8
            "BEGIN:VCARD\r\nVERSION:2.1\r\nN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:M=C3=BCller;J=C3=B6rg;;;\r\n"
            "TEL;CELL:+49 555 0101\r\nEND:VCARD\r\n")

with open(f"{out}/drawing.svg", "w") as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="96" height="64" viewBox="0 0 96 64">\n'
            '  <rect width="96" height="64" fill="#060a16"/>\n  <circle cx="30" cy="30" r="20" fill="#ff3355"/>\n'
            '  <rect x="56" y="16" width="32" height="32" fill="#2bee8a"/>\n</svg>\n')

with open(f"{out}/settings.xml", "w") as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n<settings>\n  <camera name="Lake" iso="100"/>\n'
            + "".join(f'  <shot number="{i}"/>\n' for i in range(40)) + "</settings>\n")
