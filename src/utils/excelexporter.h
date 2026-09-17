#pragma once

#include <QList>
#include <QString>
#include <QStringList>

/// 零依赖的 Excel 导出。
///
/// **格式选择：SpreadsheetML 2003（XML），扩展名 .xls。**
/// 理由：真正的 .xlsx 本质是个 zip 包（OOXML），自己实现等于把压缩、关系图、
/// 共享字符串表重造一遍；而 SpreadsheetML 2003 就是**一个纯文本 XML**，
/// 用 QXmlStreamWriter 就能写干净，Excel / WPS / LibreOffice 都能直接双击打开，
/// 且**不引入任何第三方库**。代价是列数上限 256、行数上限 65536 ——
/// 报表统计这种"每台设备一行"的场景离上限还差得远。
///
/// 注意：Excel 2016 以后对 .xls 后缀 + XML 内容会提示"格式与扩展名不匹配"，
/// 点"是"即可正常打开；想彻底避开这个提示可以存成 .xml，
/// 但 .xml 双击默认不进 Excel，两害相权还是 .xls 更方便现场使用。
namespace ExcelExport {

/// 一张工作表。
struct Sheet
{
    QString name;               ///< 工作表名（Excel 限制 31 字符，内部会自动截断）
    QString title;              ///< 首行大标题（跨列合并），可为空
    QStringList headers;        ///< 表头行
    QList<QStringList> rows;    ///< 数据行（不足的列按空处理）

    /// 哪些列按**数字**写入（下标对应 headers）。
    /// 数字单元格在 Excel 里可以直接参与求和 / 排序，比"看起来像数字的文本"有用得多。
    QList<int> numericColumns;
};

/// 写一个多工作表的 Excel 文件。失败时返回 false 并把原因写进 @p error。
bool writeWorkbook(const QString &filePath, const QList<Sheet> &sheets, QString *error);

} // namespace ExcelExport
