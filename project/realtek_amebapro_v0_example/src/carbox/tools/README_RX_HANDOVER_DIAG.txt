Wi-Fi -> TCP/IP handover 診斷 (RX_HANDOVER_DIAG=1 啟用，預設關閉)

輸出方式
  使用原有 wlan_rx_diag task (priority 1)，沒有新增 task。
  task 啟動時初始化共用 printf mutex。舊版漏接初始化，會讓 RX_* 全部略過；
    20261029174054_971B 的 audio-lag3.log 正是此情況，缺 log 不代表計數為零。
  printf/vprintf/fputc 在 ISR、IRQ 已遮罩或 scheduler 非 RUNNING 時不等待 mutex；
    此時字元可能交錯，但不會為了等其他 logger 而阻塞。正常 task 用 mutex，
    不再為整段 UART 輸出進入 critical section。初始化與此保護在診斷關閉時也保留。
  每 2 秒取樣；啟動時、異常有變化或持續未前進時輸出，正常每 10 秒輸出。
  RX 熱路徑只做固定大小的計數，不 printf、不配置記憶體、不複製封包。
  Timer callback 有 tick 計時；只有實際要等待已滿的 timer queue 才保存 task。
  新增的 RX_* 診斷先在固定大小 stack buffer 格式化，再嘗試取得共用 printf mutex。
  mutex 忙碌就略過；每一行最多 160 UART bytes（含 CRLF），釋放 mutex 後休眠 1 tick。
  長紀錄會按欄位拆行，每行重複同一 TAG 與 tick，請按 TAG/tick 合併閱讀。
  log_skip 是略過的紀錄/剩餘部分累積次數，不是丟包數；UART 忙時紀錄可能不完整。
  只送 UART，不走 CONFIG_ADD_LOG 儲存回呼；snapshot/計數不因略過輸出而清零。
  輸出仍非零成本：目前 1.5 Mbaud 下 160 bytes 線路時間約 1.07 ms，
    不含 preemption/UART 等待；沒有宣稱 thread 最長延遲保證。
  並非零成本；若要量測影響，可用相同 build 參數切 RX_HANDOVER_DIAG=0 做 A/B。
  開關有 object dependency stamp，切換時會重編相關的 kernel/lwIP/診斷檔案。
  RX_HANDOVER_DIAG 預設為 0；開啟診斷必須使用已核對的
    WLAN_RX_DMA_SKB=1 driver。關閉時 WLAN_RECVFRAME/FRAME_* 診斷也一併關閉。
  WLAN_RECVFRAME 與新增 WLAN_FRAME_* 同樣使用上述 160-byte 分行輸出。

欄位
  WLAN_RECVFRAME
    只採樣初始化時登記的 free pool；_rtw_alloc_recvframe 也用來取出等待解密
      的 frame，不能只看 caller 就把其 queue 當作 free pool。重建 pool 時重置統計。
    reorder_runs：本取樣區間觀測到的 driver reorder timeout 處理次數。
      對應本版 rtw_recv.o 的 adapter+0xca6 16-bit counter：取得 reorder lock 後、
      flush 前增加，因此不是 callback 完成數。配合 RX_TIMERQ 判斷是否仍有進展。
      隨 recvframe allocation 嘗試取樣；RX 完全停住時不能視為獨立 timer 心跳。
    free_last：最後一次 allocation 嘗試前的 free count，不是印 log 當下的快照。
    empty/total：空 pool 觀測次數/累積次數，不等於 RTP 遺失包數。

  WLAN_FRAME_POOL / WLAN_FRAME
    在已核對的 native list insert/delete 與 indicate 入口記錄生命週期，
      沒有改 driver 的排隊、排序、釋放或 timer 行為。
    held：此 pool 的 32 個 frame 中，上次觀測狀態不是 free 的數量。
      alloc/reorder/other_queue/drain/detached/indicate/defrag/decrypt 為其狀態分布。
      這些是最後觀測到的轉移，不是 reporter 重新走訪 driver list 的結果。
    oldest_ms：目前持有中最久的借用時間；不是 CPU 執行時間。
    live_mask/reorder_mask：bit N 對應 slot N；可比較是否長期卡住同一批 frame。
    totals_alloc/return：自此 epoch 開始的借出/回收次數；pending decrypt 出隊
      是恢復既有借用，不增加 alloc、不重設 age。累積值會 uint32 回繞。
    double_alloc/return：觀測到不符合狀態的重複借出/回收，增加時需調查。
    foreign：回到非原始 free queue 的次數，可能是 buddy adapter，並非直接判錯。
    unowned：free 狀態下卻觀測到非借出/回收的事件；表示需核對觀測完整性。
    missed_pools：超過四個 pool 記錄容量；非零表示觀測不完整。
    frame/caller/owner/reorder/task 都是複製的指標標籤，reporter 不解參考。
      新增的 pool balance 只在 critical section 內讀仍登記 active 的 pool/adapter。
      reorder insert 的 owner 是排序 successor，未必 queue head；control 指標
      才識別 reorder queue。task 是最初借出者，不代表目前執行/等待的 task。
    age_ms/stage_ms：從借出／最後狀態轉移到 snapshot 後取時的經過時間。
    attrs=1 才能解讀 seq80211/amsdu/frag/more；seq80211 是 12-bit Wi-Fi sequence，
      不是 RTP sequence。A-MSDU 子 frame 未初始化這些 native 欄位，故 attrs=0。
    stop_flags：reorder drain 當下 frame 所屬 adapter 的 stop/remove 位元快照。
    新 epoch 或 held>=24 或 oldest>=100 ms 時每次取樣輸出 summary；正常每10秒。
      最多4個最久持有的明細，每10秒一次。瞬間壓力仍看 WLAN_RECVFRAME.free_min。
    固定 BSS 共10724 bytes（原6036，加4688；含16筆事件、4 pools ×32 frame
      及單一 reporter snapshot）。每個 pool snapshot 複製1708 bytes metadata，
      每筆事件 snapshot 124 bytes；熱路徑無 heap/UART/封包複製。
      這仍有有限指令/critical-section 開銷，板上耗時須量測，不能宣稱零影響。
    build 前驗證實際 archive 的 rtw_recv.o/freertos_recv.o/rtl8195b_recv.o hash，
      link 後驗證22個 hook call sites、native forwarding、reporter，另驗證 saved-LR
      shim 與原生 stack prologue。ITCM/LPDDR跳板驗證指令及最終目標。

  WLAN_FRAME_FLOW / WLAN_FRAME_FLOW_Q (2026-10-29 ownership evidence)
    event=foreign_return：歸還到原 pool 之外；不單憑此認定 bug。
    event=foreign_borrow：先前歸還外部 queue 的同一個 frame 又被借出。
    event=home_return：從外部 queue 借出的 frame 歸還原 pool。
    home/epoch/slot/frame：原 pool 身分及原始32個 frame 中的位置。
    queue/queue_epoch：這次歸還的目的 queue；foreign_borrow 時則是借出來源。
      queue_epoch=0 表示不是目前登記的 free pool，不解參考未知地址。
    borrow_queue：該次 loan 的借出 queue，根據上次觀測的 free destination 保存。
      不根據 frame->adapter 猜測；無法建立合法借出轉移時為0。
    alloc_adapter：借出瞬間 frame 原有 adapter，可能是前次使用留下的值。
    entry_adapter/entry_pc：進入 rtw_recv_entry 前的 adapter 與實際呼叫返回位址；
      對照同版本 ELF，可分辨原始/clone/buddy 路徑。沒有經此入口則為0，
      例如 native A-MSDU child 不會再次經過這個入口。
    adapter_at_hook/buddy/adapter_type/promisc：歸還或再次借出時的 adapter 快照。
      type 是 native byte(+0x338a)，promisc 是 native byte(+0xd84)，保留raw值。
      只解參考已登記 active adapter；未知時 type/promisc=4294967295。
    free_pc：真正呼叫 rtw_free_recvframe 的返回位址（已清Thumb bit），不是
      list_insert wrapper 自己的返回點。可以涵蓋 native 同一個 .o 內部的呼叫。
      非 native free 的 helper 路徑及 foreign_borrow 為0，不猜測 caller。
      若呼叫端使用tail call，保存的LR可能屬更上層；須配prev_pc/entry_pc及反組譯判讀。
    prev_state/prev_pc：該次事件前的最後觀測狀態/轉移位置；state編號為
      0free,1alloc,2reorder,3queue,4drain,5detached,6indicate,7defrag,8decrypt。
    home_count_at_hook/queue_count_at_hook：hook當下的 native count；list insert/delete
      已執行，但 native 的加一/減一尚未執行。不可當作整個free/alloc返回後的值。
      未登記 queue 的 count=4294967295。穩態數量看 WLAN_POOL_BALANCE。
    id 是事件識別碼，跨行重複，tick 是事件發生時間；輸出可能晚於事件。
      事件是值拷貝，pool重建後仍保留舊epoch，不解參考已釋放物件。
    固定16筆事件，滿時保留既有紀錄並累加lost；每次既有統計task最多輸出2筆。
      FLOW_Q顯示queued/lost；id跳號、lost或RX_HANDOVER.log_skip增加表示證據不完整。
      UART mutex忙時仍可能略過部分/整筆輸出，不把「沒有印出」當作「沒有發生」。

  WLAN_POOL_BALANCE
    native_free：登記queue的原生free count，保留大於32的值，以觀察外來frame流入。
    home_free/foreign_free/held：原始32個frame中，最後觀測為在原free queue、
      在外部free queue、以及仍借出中的數量。三者加總為32。
    foreign_mask：哪些slot的最後free destination不在原pool。
    foreign_oldest_ms：這些frame中最久未再次借出的時間，驗證是否持續滯留。
    native_free包含其他pool的frame，home_free只計本pool原始frame；不必相等。
      這些是受鎖保護的當下快照，不走訪native linked list；初始化/借還轉移中
      的短暫差異不能單獨當成帳目損壞。配合多次快照及flow events判斷。

  觀測邊界
    只收集證據，未變更queue選擇、frame回收策略、CPU頻率、zero-copy門檻。
    saved-LR shim僅在原生rtw_free_recvframe+0x46呼叫點讀SP+28；其他caller不讀。
      Native hash/prologue及最終shim指令不符時build會停止，避免ABI改版讀錯stack。
    這是有限開銷的診斷版；host測試/反組譯核對不等同板上效能或根因已確認。

  RX_HANDOVER
    delta_in/out/msg/timer：上一個 sample_ms 期間的交付數、WLAN 處理完成數、
      TCP/IP message 完成數、FreeRTOS timer command 取出數。
      正常每 10 秒才印，但 delta 仍只代表最後一次約 2 秒的取樣。
    totals_in/copy/zc/out：開機至今的 ethernetif_recv 進入數、copy 成功數、
      zero copy 成功數、TCP/IP consumer 完成 WLAN packet 處理數。
      out 不代表音訊已播放，也不保證 IP/UDP 層沒有丟包。
    pbuf_fail/copy_fail：WLAN RTP 觀測點之前的配置或搬運失敗。
    input_reject：netif->input 回傳錯誤；ethernetif 已釋放該 pbuf。
    zc_held：目前仍被引用的 zero copy wrapper，不直接等於 leak。
    no_progress：相鄰兩次取樣都有 backlog，對應 consumer 計數沒有增加。
      是追查線索；不是已確認 deadlock。

  RX_BATCH
    mbox_now：TCP/IP mailbox 當下深度；不是區間峰值。
    in_use：128 個 WLAN stage entry 中目前使用數，包含 pending/queued/processing。
    pending：還沒有成功送進 mailbox 的 batch packets。
    armed/available：1 ms 硬體 GTimer 是否已啟動、是否初始化成功。
    max_batch：實際 dispatch 的最大 batch。正常 flush 門檻是 8；滿載恢復時
      合法 batch 可以大於 8，上限為 entry pool 128。
    pool_empty：stage pool 不足次數，可能走普通 envelope fallback，未必丟包。
    memp_fail/mbox_fail：普通 envelope 配置失敗、普通/單包入列失敗。
    post_fail_task/isr：batch post 失敗；已接受的 pbuf 仍保留，不直接算丟包。
    retry/ok：consumer 釋放 mailbox slot 後，補送未 armed 的 pending batch 次數/成功數。
    bad：batch 結構或長度不符，正常應為 0。
    stack_reject：consumer input_fn 回傳錯誤；consumer 已釋放 pbuf。
      這些失敗計數是累積值，普通 envelope/stack_reject 可包含其他 netif。

  RX_NCM
    totals_in/out：USB-NCM FIFO 接受/處理完成的累積 packet 數。
    pool_fail/post_fail：NCM pool 不足、mailbox marker post 失敗。
    marker：NCM drain 已排入 mailbox 或正在執行。
    NCM out 持續增長、WLAN out 停住且 in_use 不退，可追查 NCM drain 的排程佔用。

  RX_TIMERQ / RX_TIMER_WAIT
    與 RX_BATCH 的硬體 GTimer 不同，這是 Wi-Fi reorder 使用的 FreeRTOS timer queue。
    depth：目前 queue 深度/容量；processed 是累積取出 command 數。
    full_wait：實際進入滿 queue 等待的次數；self 表示 timer daemon 自己等待。
    fail：timer command/pended function post 回傳失敗數，含非阻塞及 ISR。
    waiting：目前記錄中尚未從 send 返回的 task 數，最多 4 個。
    wait_overflow：等待 task 超過記錄容量；增加時表示觀測不完整。
    cb_start/done、cb_active、cb_age/max_ms：callback 開始/結束數、進行中狀態、
      目前 callback 年齡及累積最大執行經過時間。包含 preemption/blocking 時間。
    timer/function/daemon：指標供對照此版 ELF；不在 reporter 解參考已釋放的物件。
    RX_TIMER_WAIT 保存 task 名稱/handle、command ID、timer、開始等待後的經過時間、
      wait_ticks 及是否 daemon 自己。UINT32_MAX 是無限等待。
    send 返回、或 vTaskDelete 刪除 task 時清除等待紀錄，避免 TCB 重用留下假的等待。
    snapshot 後 task 仍可能返回或被刪除，所以 handle/name 是取樣當下的資料。

怎麼判讀
  1. 要保留完整開機版本與從正常到斷音之後至少 30 秒的原始 UART log。
     註記實際聽到斷音的時間、iPhone 型號，以及音樂/導航哪一路有問題。
  2. input_reject 增加，配合 RX_BATCH 的 pool/memp/mbox 失敗判斷交接丟包。
  3. post_fail 增加後 retry/ok 增加、in_use/pending 下降，表示滿載後有恢復。
  4. Timer queue 滿、processed 不動，且 self wait 或 callback age 持續增加，
     才有具體證據追查 reorder/timer command 互等；目前反組譯只確認可能路徑。
  5. 若這些計數正常但 AUDIO_WLAN_PRE 已有 RTP sequence 缺口，須往該觀測點之前
     的 driver/reorder/RF/發送端追查；不能直接把原因定為 RF 或 iPhone。
  6. 不同來源分別取 snapshot，跨來源可能差幾個 in-flight packets。
     各 counter 是 uint32 累積值，長跑有回繞；delta 使用無號減法。
  7. Reporter 是低優先序；若 CPU/IRQ 完全卡死或長時間高優先序忙迴圈，log 也可能停止。
  8. held 接近32且 reorder 的同一組 slot/age 持續增加：追查等待缺失序號與
     reorder timer 是否仍前進。defrag 多：追查未完成的分片；不直接認定洩漏。
     indicate 多且持續不退：追查 driver→netif 交付是否阻塞。
     detached/drain 長期不退或 double_return 增加：追查未歸還/錯誤歸還路徑。

本次一併保留的滿載修正
  tcpip.c consumer 允許合法的 9..128 packet batch。
  一次性 GTimer post 因 mailbox 滿而失敗後，由 consumer 補送，無須等下一包 RX。
  RX_HANDOVER_DIAG=0 只移除診斷，仍保留上述修正。

Host 驗證
  python3 test_tcpip_rx_batch.py
    生產碼 batch/consumer/enqueue，log 開關各 9 個情境，檢查 FIFO、pbuf 所有權和恢復。
  python3 test_rx_handover_timer.py
    生產碼 timer hooks/command，模擬 queue 滿、self wait、ISR、多 task 返回，
    以及刪除所有等待者後的 slot/TCB 重用。未執行真實 RTOS scheduler。
  python3 test_rx_handover_report.py
    生產碼診斷 task、mutex 初始化及 reporter；從 NULL mutex 啟動，確認實際輸出。
    檢查 UART 每行長度、mutex 不等待、先解鎖再休眠、
    最大計數不截斷、輸出忙碌時略過與恢復；ROM formatter/UART 使用 host stub。
  python3 test_printf_lock.py
    生產碼 printf lock/unlock；覆蓋 early boot、初始化、ISR、IRQ 遮罩、
    scheduler 暫停/未啟動，以及依取得模式正確解鎖。ARM register 讀取用 stub。
  python3 test_recvframe_trace.py
    生產碼生命週期 hooks；借還/重複操作、32個全被占用、明細限流、decrypt出隊
    保留age、defrag兩種插入、A-MSDU無效metadata、pool重建、容量、跨pool回收及tick回繞。
    另驗證foreign→再次借出→回原pool、generic enqueue到foreign free queue、
    native count>32、未知queue/adapter、事件滿載/限流/epoch保存、最長格式、
    以及native entry消耗/重新借出frame後wrapper不再寫入新loan。
  python3 verify_recvframe_trace.py --prefix <toolchain-prefix> --archive <lib_wlan_zero_copy.a> --elf <application_is.axf>
    實際 native member 與最終連結目標核對；build 已自動執行。
  Host 測試不代替板上長時間播放與效能量測。

audio-lag3.log / native review 結論 (2026-10-29)
  原檔 /home/kevin/audio-lag3.log，build 20261029174054_971B。
  140.721秒首次觀測空pool；152.847秒起49個取樣區間的free_last均為0或1，
    到248.951秒仍持續。同期有300ms RTP間隙、實際midstream rebuffer與音樂unrecovered。
    recovered仍增加，表示有繼續回收/借用；不能說pool永久完全不回收。
  AAC-ELD所有132筆視窗 errors=0、over_frame_budget=0、最高max_us=1570，
    尚無decoder單次執行超過10884us的證據；不等於證明所有task/IRQ沒有延遲。
  正常 rtw_recv_indicatepkt 返回前會回收recvframe。zero copy可保留skb/pbuf，
    並不直接把32個recvframe留在TCP/IP socket。因此CPU有餘裕仍可能因
    reorder/defrag等持有者等待條件而讓pool耗盡。
  靜態發現、尚無本次runtime證據的兩條路徑（本次僅觀測，未修改vendor binary）：
    - recvframe_defrag+0x42釋放後續片，回傳第一片；recv_func_posthandle+0x5c
      在重組後被reorder拒收時卻free原始最後片，可能重複回收最後片並遺失第一片。
      必須同時有Wi-Fi分片重組與下游拒收；一般無分片的拒收不會走到此問題。
    - recv_indicatepkts_in_order+0x8c已detach；若stop/remove旗標非零，
      +0xa6..0xb0分支略過indicate/free。一般正常旗標為零時不走這條路。
  新log用來驗證實際持有狀態；不把以上靜態發現直接當作iPhone15斷音根因。

覆查修正 (2026-10-29)
  1. vTaskDelete 清除 timer wait record；保留 timer queue 原來的阻塞／回傳語意。
  2. 新診斷輸出限制單次 mutex 持有的字元數，競爭時略過，不在鎖內做格式化。
  3. 普通 TCPIP_MSG_INPKT 路徑的 WLAN out 改在 input_fn 返回後才累加，
     與 batch/stage consumer 一致，避免把正在處理中的 packet 算成已完成。
  4. 滿載時最多 128 包的 batch 會延長單次 consumer 處理時間；正常門檻仍為 8。
     此限制及 NCM drain 的排程影響須在板上觀測，不能由 host 測試推定無延遲。
