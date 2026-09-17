#include "utils/excelexporter.h"

#include <QFile>
#include <QXmlStreamWriter>

namespace ExcelExport {

namespace {

/// Excel 的工作表名限制：≤31 字符，且不能含 : \ / ? * [ ]
QString sanitizeSheetName(const QString &name, int index)
{
    QString cleaned = name.trimmed();
    if (cleaned.isEmpty())
        cleaned = QStringLiteral("Sheet%1").arg(index + 1);

    static const QString forbidden = QStringLiteral(":\\/?*[]");
    for (const QChar &ch : forbidden)
        cleaned.replace(ch, QLatin1Char('_'));

    return cleaned.left(31);
}

/// 数字列判定：能解析成数字、且不是空串的才按数字写。
/// 报表里的"—"（无数据）必须留在文本里，否则 Excel 会把它变成 0。
bool isNumeric(const QString &text, double *out)
{
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (ok && out)
        *out = value;
    return ok;
}

void writeCell(QXmlStreamWriter &xml, const QString &text, bool numeric)
{
    double value = 0.0;
    const bool asNumber = numeric && isNumeric(text, &value);

    xml.writeStartElement(QStringLiteral("Cell"));
    if (asNumber)
        xml.writeAttribute(QStringLiteral("ss:StyleID"), QStringLiteral("sNumber"));
    else if (!text.isEmpty())
        xml.writeAttribute(QStringLiteral("ss:StyleID"), QStringLiteral("sText"));

    xml.writeStartElement(QStringLiteral("Data"));
    xml.writeAttribute(QStringLiteral("ss:Type"), asNumber ? QStringLiteral("Number")
                                                           : QStringLiteral("String"));
    xml.writeCharacters(asNumber ? QString::number(value, 'g', 15) : text);
    xml.writeEndElement(); // Data
    xml.writeEndElement(); // Cell
}

void writeStyles(QXmlStreamWriter &xml)
{
    xml.writeStartElement(QStringLiteral("Styles"));

    xml.writeStartElement(QStringLiteral("Style"));
    xml.writeAttribute(QStringLiteral("ss:ID"), QStringLiteral("sTitle"));
    xml.writeStartElement(QStringLiteral("Font"));
    xml.writeAttribute(QStringLiteral("ss:Bold"), QStringLiteral("1"));
    xml.writeAttribute(QStringLiteral("ss:Size"), QStringLiteral("14"));
    xml.writeEndElement();
    xml.writeEndElement();

    xml.writeStartElement(QStringLiteral("Style"));
    xml.writeAttribute(QStringLiteral("ss:ID"), QStringLiteral("sHeader"));
    xml.writeStartElement(QStringLiteral("Font"));
    xml.writeAttribute(QStringLiteral("ss:Bold"), QStringLiteral("1"));
    xml.writeAttribute(QStringLiteral("ss:Color"), QStringLiteral("#FFFFFF"));
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("Interior"));
    xml.writeAttribute(QStringLiteral("ss:Color"), QStringLiteral("#2F7BD6"));
    xml.writeAttribute(QStringLiteral("ss:Pattern"), QStringLiteral("Solid"));
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("Alignment"));
    xml.writeAttribute(QStringLiteral("ss:Horizontal"), QStringLiteral("Center"));
    xml.writeAttribute(QStringLiteral("ss:Vertical"), QStringLiteral("Center"));
    xml.writeEndElement();
    xml.writeEndElement();

    xml.writeStartElement(QStringLiteral("Style"));
    xml.writeAttribute(QStringLiteral("ss:ID"), QStringLiteral("sText"));
    xml.writeStartElement(QStringLiteral("Alignment"));
    xml.writeAttribute(QStringLiteral("ss:Vertical"), QStringLiteral("Center"));
    xml.writeEndElement();
    xml.writeEndElement();

    xml.writeStartElement(QStringLiteral("Style"));
    xml.writeAttribute(QStringLiteral("ss:ID"), QStringLiteral("sNumber"));
    xml.writeStartElement(QStringLiteral("NumberFormat"));
    xml.writeAttribute(QStringLiteral("ss:Format"), QStringLiteral("General"));
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("Alignment"));
    xml.writeAttribute(QStringLiteral("ss:Horizontal"), QStringLiteral("Right"));
    xml.writeEndElement();
    xml.writeEndElement();

    xml.writeEndElement(); // Styles
}

void writeSheet(QXmlStreamWriter &xml, const Sheet &sheet, int index)
{
    const int columnCount = qMax(1, sheet.headers.size());

    xml.writeStartElement(QStringLiteral("Worksheet"));
    xml.writeAttribute(QStringLiteral("ss:Name"), sanitizeSheetName(sheet.name, index));

    xml.writeStartElement(QStringLiteral("Table"));

    // 列宽：给所有列一个统一的舒适宽度，省得打开后到处拖列宽
    for (int column = 0; column < columnCount; ++column) {
        xml.writeStartElement(QStringLiteral("Column"));
        xml.writeAttribute(QStringLiteral("ss:Width"), QStringLiteral("130"));
        xml.writeEndElement();
    }

    if (!sheet.title.isEmpty()) {
        xml.writeStartElement(QStringLiteral("Row"));
        xml.writeAttribute(QStringLiteral("ss:Height"), QStringLiteral("26"));

        xml.writeStartElement(QStringLiteral("Cell"));
        xml.writeAttribute(QStringLiteral("ss:MergeAcross"), QString::number(columnCount - 1));
        xml.writeAttribute(QStringLiteral("ss:StyleID"), QStringLiteral("sTitle"));
        xml.writeStartElement(QStringLiteral("Data"));
        xml.writeAttribute(QStringLiteral("ss:Type"), QStringLiteral("String"));
        xml.writeCharacters(sheet.title);
        xml.writeEndElement(); // Data
        xml.writeEndElement(); // Cell

        xml.writeEndElement(); // Row
    }

    if (!sheet.headers.isEmpty()) {
        xml.writeStartElement(QStringLiteral("Row"));
        xml.writeAttribute(QStringLiteral("ss:Height"), QStringLiteral("22"));
        for (const QString &header : sheet.headers) {
            xml.writeStartElement(QStringLiteral("Cell"));
            xml.writeAttribute(QStringLiteral("ss:StyleID"), QStringLiteral("sHeader"));
            xml.writeStartElement(QStringLiteral("Data"));
            xml.writeAttribute(QStringLiteral("ss:Type"), QStringLiteral("String"));
            xml.writeCharacters(header);
            xml.writeEndElement(); // Data
            xml.writeEndElement(); // Cell
        }
        xml.writeEndElement(); // Row
    }

    for (const QStringList &row : sheet.rows) {
        xml.writeStartElement(QStringLiteral("Row"));
        for (int column = 0; column < columnCount; ++column) {
            const QString text = column < row.size() ? row.at(column) : QString();
            writeCell(xml, text, sheet.numericColumns.contains(column));
        }
        xml.writeEndElement(); // Row
    }

    xml.writeEndElement(); // Table
    xml.writeEndElement(); // Worksheet
}

} // namespace

bool writeWorkbook(const QString &filePath, const QList<Sheet> &sheets, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (sheets.isEmpty())
        return fail(QStringLiteral("没有可导出的内容"));

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("无法写入文件：%1").arg(file.errorString()));
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(1);

    xml.writeStartDocument(QStringLiteral("1.0"));

    // 这一条处理指令是 Excel 的"暗号"：看到它才会把文件认成工作簿而不是普通 XML
    xml.writeProcessingInstruction(QStringLiteral("mso-application"),
                                   QStringLiteral("progid=\"Excel.Sheet\""));

    xml.writeStartElement(QStringLiteral("Workbook"));
    xml.writeAttribute(QStringLiteral("xmlns"), QStringLiteral("urn:schemas-microsoft-com:office:spreadsheet"));
    xml.writeAttribute(QStringLiteral("xmlns:o"), QStringLiteral("urn:schemas-microsoft-com:office:office"));
    xml.writeAttribute(QStringLiteral("xmlns:x"), QStringLiteral("urn:schemas-microsoft-com:office:excel"));
    xml.writeAttribute(QStringLiteral("xmlns:ss"), QStringLiteral("urn:schemas-microsoft-com:office:spreadsheet"));
    xml.writeAttribute(QStringLiteral("xmlns:html"), QStringLiteral("http://www.w3.org/TR/REC-html40"));

    writeStyles(xml);

    int index = 0;
    for (const Sheet &sheet : sheets)
        writeSheet(xml, sheet, index++);

    xml.writeEndElement(); // Workbook
    xml.writeEndDocument();

    if (xml.hasError()) {
        file.close();
        return fail(QStringLiteral("写入 Excel XML 时出错"));
    }

    file.close();
    return true;
}

} // namespace ExcelExport
