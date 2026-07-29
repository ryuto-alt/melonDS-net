/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "TermsDialog.h"
#include "Config.h"

TermsDialog::TermsDialog(QWidget* parent, bool gate) : QDialog(parent)
{
    setWindowTitle("RyuE 利用規約");
    resize(860, 680);

    auto* browser = new QTextBrowser(this);
    browser->setOpenExternalLinks(true);
    browser->document()->setDefaultStyleSheet(
        "body { line-height: 150%; }"
        "h1 { font-size: 17pt; }"
        "h2 { font-size: 12pt; margin-top: 18px; }"
        "li { margin-bottom: 5px; }"
        ".warn { background-color: #4a1f1f; border: 1px solid #a04040; padding: 8px; }"
        ".note { background-color: #2b2b33; border: 1px solid #555; padding: 8px; }");
    browser->setHtml(html());
    browser->moveCursor(QTextCursor::Start);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(browser);

    auto* buttons = new QDialogButtonBox(this);

    if (gate)
    {
        auto* agree = new QCheckBox("上記の利用規約をすべて読み、内容に同意します。", this);
        layout->addWidget(agree);

        QPushButton* ok = buttons->addButton("同意して起動", QDialogButtonBox::AcceptRole);
        buttons->addButton("同意しない（終了）", QDialogButtonBox::RejectRole);
        ok->setEnabled(false);
        connect(agree, &QCheckBox::toggled, ok, &QPushButton::setEnabled);
    }
    else
    {
        buttons->addButton("閉じる", QDialogButtonBox::RejectRole);
    }

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool TermsDialog::requireAcceptance()
{
    Config::Table cfg = Config::GetGlobalTable();
    if (cfg.GetInt("TermsAcceptedVersion") >= kVersion)
        return true;

    TermsDialog dlg(nullptr, true);
    if (dlg.exec() != QDialog::Accepted)
        return false;

    cfg.SetInt("TermsAcceptedVersion", kVersion);
    Config::Save();
    return true;
}

QString TermsDialog::html()
{
    return QString::fromUtf8(R"HTML(
<h1>RyuE 利用規約および法的注意事項</h1>
<p><small>規約バージョン 1 ／ 制定日: 2026年7月29日</small></p>

<div class="warn">
<p><b>最初に必ずお読みください。</b></p>
<p>RyuE は<b>ニンテンドーDS のエミュレータ</b>です。ゲームソフト（ROM イメージ）、BIOS、ファームウェアは
<b>一切含まれておらず、配布もしていません</b>。これらは利用者ご自身が、
<b>ご自身が適法に所有する実機および正規に購入したソフトから吸い出したもの</b>のみをお使いください。</p>
<p>インターネット上で配布されている ROM・BIOS・ファームウェアを入手する行為は、
日本の著作権法に違反し、<b>刑事罰の対象となる場合があります</b>。
また、吸い出したデータを他人に渡す行為は、<b>相手が友人であっても、無償であっても、著作権侵害です</b>。</p>
</div>

<h2>第1条（定義）</h2>
<ol>
<li>「本ソフトウェア」とは、melonDS を基とする派生ソフトウェアである RyuE、およびその実行ファイル、ソースコード、付随する資料をいいます。</li>
<li>「開発者」とは、本ソフトウェアの開発および配布を行う者をいいます。</li>
<li>「ROM イメージ」とは、ニンテンドーDS／ニンテンドーDSi 用ゲームソフトのプログラムおよびデータを電子的に複製したものをいいます。</li>
<li>「BIOS 等」とは、実機に搭載された BIOS、ファームウェア、暗号鍵その他のシステムプログラムおよびそれらを複製したものをいいます。</li>
<li>「利用者」とは、本ソフトウェアをダウンロード、複製、実行、改変、または再頒布する者をいいます。</li>
</ol>

<h2>第2条（本規約への同意）</h2>
<ol>
<li>利用者は、本ソフトウェアを使用する前に、本規約の全文を読み、その内容に同意しなければなりません。</li>
<li>初回起動時に表示される本画面で同意した時点で、利用者と開発者との間に本規約が成立します。</li>
<li>本規約に同意されない場合は、本ソフトウェアを使用せず、直ちに削除してください。</li>
</ol>

<h2>第3条（本ソフトウェアの性質）</h2>
<ol>
<li><b>非公式かつ無関係であること。</b>本ソフトウェアは、任天堂株式会社（以下「任天堂」）およびその関連会社、その他一切の権利者とは無関係な、非公式かつ独立したソフトウェアです。任天堂その他の権利者による出資、承認、後援、提携、監修は一切受けていません。</li>
<li><b>権利者の著作物を含まないこと。</b>本ソフトウェアには、任天堂その他の権利者が著作権を有するプログラムコード、BIOS、ファームウェア、暗号鍵、ゲームソフトのプログラムおよびデータは、<b>一切含まれていません</b>。</li>
<li><b>独自実装であること。</b>本ソフトウェアは、公開された技術情報および相互運用性の確保を目的とする適法な解析に基づき、独自に記述されたプログラムです。米国においては
Sega Enterprises Ltd. v. Accolade, Inc., 977 F.2d 1510 (9th Cir. 1992) および
Sony Computer Entertainment, Inc. v. Connectix Corp., 203 F.3d 596 (9th Cir. 2000)
が、この種の解析および中間的複製をフェアユースとして是認しています。</li>
<li><b>一切の頒布を行わないこと。</b>開発者は、ROM イメージ、BIOS 等、暗号鍵を、いかなる方法によっても配布、提供、送信、頒布しません。また、それらの入手先を案内、紹介、教示することもありません。</li>
<li><b>回避手段を提供しないこと。</b>開発者は、暗号鍵の抽出、技術的保護手段の解除、または不正な複製物の作成を目的とする装置、プログラム、手順を提供しません。</li>
<li><b>海賊行為を助長しないこと。</b>本ソフトウェアは、実機およびソフトを適法に所有する利用者が、自らの所有物を自らの環境で動作させるための互換環境として提供されるものであり、著作権侵害を目的または主たる用途とするものではありません。</li>
</ol>

<h2>第4条（利用者の遵守事項）</h2>

<p><b>4-1　自己吸い出しの原則</b><br>
利用者は、本ソフトウェアで使用する ROM イメージおよび BIOS 等について、次のすべてを満たさなければなりません。</p>
<ol>
<li>当該ゲームソフトを正規に購入し、現に所有していること。</li>
<li>当該実機（ニンテンドーDS 本体等）を適法に所有していること。</li>
<li>それらのデータを、<b>利用者自身が、自らの所有物から吸い出したもの</b>であること。</li>
</ol>

<p><b>4-2　BIOS・ファームウェアについて</b><br>
BIOS 等は、<b>必ず利用者ご自身が所有する実機から吸い出してください。</b>
インターネット上で配布されているものを入手・使用してはなりません。
開発者は BIOS 等を提供せず、その入手方法についても案内しません。</p>

<p><b>4-3　禁止事項</b><br>
利用者は、次の行為を<b>絶対に行ってはなりません</b>。</p>
<ol>
<li>ROM イメージまたは BIOS 等を、第三者に提供、譲渡、貸与、販売、送信、アップロード、共有すること。SNS、Discord、オンラインストレージ、ファイル共有ソフト、電子メール、物理媒体の受け渡しを含め、方法の一切を問いません。<b>友人・知人に渡す行為、無償で渡す行為も、これに含まれます。</b></li>
<li>インターネットその他から、権利者の許諾なく公開された ROM イメージまたは BIOS 等をダウンロードし、または入手すること。</li>
<li>私的使用（著作権法第30条）の範囲を超えて、ROM イメージまたは BIOS 等を複製すること。</li>
<li>技術的保護手段または技術的制限手段を回避して、複製物を作成すること。</li>
<li>本ソフトウェアを、海賊版の実行、頒布、宣伝、その他著作権侵害を助長し、または幇助する目的で使用すること。</li>
<li>対応するゲームソフトまたは実機を譲渡、売却、廃棄した後も、そこから吸い出したデータを保持し、または使用すること。</li>
<li>本ソフトウェアに ROM イメージまたは BIOS 等を同梱して再頒布すること。</li>
<li>本ソフトウェアが権利者の公式製品であるかのように誤認させる表示、宣伝、販売を行うこと。</li>
<li>本ソフトウェアを有償で販売し、またはプリインストールした機器を販売すること（GPLv3 に基づく実費相当額の請求を除きます）。</li>
<li>その他、日本国またはご利用の国・地域の法令に違反する一切の行為。</li>
</ol>

<p><b>4-4　ネットプレイ機能に関する特則</b><br>
本ソフトウェアのネットプレイ機能は、対戦を成立させるために、
ホストの実行中データ（ROM イメージおよび BIOS 等を含みます）を参加者の端末へ一時的に送信する場合があります。
この送信は当該セッションを成立させるためにのみ行われるものですが、これは
<b>参加者の全員が当該ゲームソフトおよび実機を適法に所有していること</b>を当然の前提とします。
利用者は、次を遵守しなければなりません。</p>
<ol>
<li>自らが適法に所有しないゲームのセッションに参加しないこと。</li>
<li>当該ゲームソフトを所有しない者を、自らのセッションに参加させないこと。</li>
<li>セッション中に送受信されたデータを、抽出、保存、複製、再頒布しないこと。</li>
</ol>
<p>これに反する使用は、公衆送信権（著作権法第23条）の侵害となるおそれがあります。</p>

<h2>第5条（法令上の注意）</h2>
<p>以下は一般的な情報提供であり、法的助言ではありません。個別の判断については弁護士等の専門家にご相談ください。</p>

<p><b>5-1　日本</b></p>
<ul>
<li><b>著作権法第30条第1項（私的使用のための複製）</b>：個人的に、または家庭内その他これに準ずる限られた範囲内で使用することを目的とする複製は、原則として適法です。<b>ただし、次の場合は除外されます。</b>
  <ul>
  <li>同項第2号：技術的保護手段の回避により可能となった複製を、その事実を知りながら行う場合。</li>
  <li>同項第3号・第4号（令和2年改正、2021年1月1日施行）：著作権を侵害して公衆送信された著作物を、侵害著作物であると知りながらダウンロードする場合。</li>
  </ul>
</li>
<li><b>罰則</b>（2025年6月1日施行の刑法改正により、「懲役」は「拘禁刑」に改められています）
  <ul>
  <li>第119条第1項：複製権・公衆送信権等の侵害 →
      <b>10年以下の拘禁刑もしくは1000万円以下の罰金、またはその併科</b>。
      法人には第124条により<b>3億円以下の罰金</b>。</li>
  <li>第119条第3項第2号：侵害コンテンツのダウンロード（有償著作物等を継続的または反復して行った場合） →
      <b>2年以下の拘禁刑もしくは200万円以下の罰金、またはその併科</b>。</li>
  <li>第120条の2：技術的保護手段の回避を可能とする装置・プログラムの公衆への譲渡・提供等 →
      <b>3年以下の拘禁刑もしくは300万円以下の罰金、またはその併科</b>。</li>
  </ul>
</li>
<li><b>民事責任</b>：権利者は、差止請求（著作権法第112条）および損害賠償請求（民法第709条、著作権法第114条）を行うことができます。</li>
<li><b>不正競争防止法</b>：第2条第1項第17号・第18号により、技術的制限手段の効果を妨げる装置・プログラムの提供は不正競争行為に該当し、差止め・損害賠償（同法第3条・第4条）および同法第21条の刑事罰の対象となり得ます。</li>
</ul>

<p><b>5-2　アメリカ合衆国</b></p>
<ul>
<li><b>17 U.S.C. §1201（DMCA・技術的保護手段の回避規制）</b>：暗号化等のアクセス制御を回避する行為、および回避手段を提供する行為は違法とされています。</li>
<li>2024年、任天堂は Nintendo Switch エミュレータ「yuzu」の開発元 Tropic Haze LLC を同条違反等で提訴し、同社は約240万米ドルの支払いと開発中止に応じて和解しました（Nintendo of America Inc. v. Tropic Haze LLC, D.R.I. 2024）。同事件で問題とされたのは<b>エミュレータそれ自体ではなく、権利者の暗号鍵を用いた復号（回避）と海賊行為の助長</b>である点にご注意ください。</li>
<li>ROM 配布サイトに対しても、任天堂は多額の賠償を得ています（例：Nintendo v. Storman（RomUniverse）, C.D. Cal. 2021、約210万米ドル）。</li>
</ul>

<p><b>5-3　欧州連合</b></p>
<ul>
<li>指令 2009/24/EC 第6条は、相互運用性の確保に必要な範囲でのデコンパイルを許容しています。</li>
<li>他方、Nintendo Co. Ltd v. PC Box Srl（欧州司法裁判所 C-355/12, 2014年）は、技術的保護手段を回避する装置の提供が違法となり得ることを示しています。</li>
</ul>

<p><b>5-4　その他の国・地域</b><br>
著作権法および回避規制の内容は国により異なります。利用者は、自らの居住地および使用地の法令を確認し、これを遵守する責任を負います。</p>

<h2>第6条（本規約と GNU GPLv3 との関係）</h2>
<div class="note">
<ol>
<li>本ソフトウェアは、GNU General Public License version 3（以下「GPLv3」）に基づいて頒布される自由ソフトウェアです。</li>
<li><b>本規約は、GPLv3 が利用者に付与する使用・複製・改変・再頒布の権利を制限するものではありません。</b>GPLv3 第7条により、本規約は GPLv3 に対する追加的制限として解釈されてはならず、両者が抵触する場合には GPLv3 が優先します。</li>
<li>第4条の遵守事項は、GPLv3 上の権利に対する制限ではなく、<b>適用法令上当然に違法となる行為についての注意喚起および確認</b>です。</li>
<li>本ソフトウェアを再頒布する場合、GPLv3 に従い、対応するソースコードの提供、ならびに著作権表示およびライセンス表示の保持が必要です。</li>
</ol>
</div>

<h2>第7条（無保証および免責）</h2>
<ol>
<li>本ソフトウェアは「現状有姿」で提供され、商品性、特定目的への適合性、権利非侵害を含む、明示または黙示のいかなる保証も行われません（GPLv3 第15条）。</li>
<li>開発者は、本ソフトウェアの使用または使用不能から生じる一切の損害（データの消失、セーブデータの破損、実機・周辺機器の損傷、逸失利益、第三者との紛争を含みます）について、適用法令が許す最大限の範囲で責任を負いません（GPLv3 第16条）。</li>
<li><b>利用者が第4条に違反したことにより生じた一切の責任（民事上、刑事上、行政上の責任を含みます）は、当該利用者が単独で負うものとし、開発者は一切の責任を負いません。</b></li>
<li>利用者が第4条に違反したことにより開発者が第三者から請求、訴訟その他の申立てを受けた場合、利用者は、自らの費用と責任においてこれを解決し、開発者に生じた損害（合理的な弁護士費用を含みます）を補償するものとします。</li>
</ol>

<h2>第8条（商標）</h2>
<p>「Nintendo」「ニンテンドー」「任天堂」「Nintendo DS」「ニンテンドーDS」「Nintendo DSi」およびこれらに関連する名称・ロゴは、任天堂株式会社の商標または登録商標です。
本規約および本ソフトウェアにおけるこれらの表示は、対応するハードウェアを識別するための記述的使用にとどまり、商標権者との提携、承認、後援の関係を示すものではありません。
その他の商品名・会社名は、各社の商標または登録商標です。</p>

<h2>第9条（権利者からの申立て）</h2>
<p>開発者は、権利者その他の第三者から本ソフトウェアに関する適法な申立てを受けた場合、誠実にこれに対応します。
ご連絡は、本ソフトウェアの配布ページに記載の窓口までお願いいたします。</p>

<h2>第10条（本規約の変更）</h2>
<ol>
<li>開発者は、本規約を変更することがあります。</li>
<li>本規約の内容に実質的な変更があった場合には、次回起動時に本画面が改めて表示され、再度の同意を求めます。</li>
<li>本ソフトウェアの単なるバージョンアップでは、再度の同意を求めません。</li>
<li>同意済みの本規約は、いつでも「ヘルプ」メニューの「利用規約...」から確認できます。</li>
</ol>

<h2>第11条（分離可能性）</h2>
<p>本規約のいずれかの条項が無効または執行不能と判断された場合でも、その他の条項の効力は影響を受けません。</p>

<h2>第12条（準拠法および管轄）</h2>
<p>本規約は日本法に準拠します。本規約または本ソフトウェアに関して紛争が生じた場合には、東京地方裁判所を第一審の専属的合意管轄裁判所とします。</p>

<hr>

<div class="warn">
<p><b>まとめ ─ これだけは必ず守ってください</b></p>
<ol>
<li>ROM も BIOS も、<b>自分の実機・自分のソフトから、自分で吸い出す。</b></li>
<li>吸い出したデータは、<b>誰にも渡さない。</b></li>
<li>ネットからは、<b>絶対に落とさない。</b></li>
</ol>
</div>
)HTML");
}
