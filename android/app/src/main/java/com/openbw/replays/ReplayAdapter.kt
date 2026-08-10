package com.openbw.replays

import android.text.format.DateUtils
import android.text.format.Formatter
import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import com.openbw.replays.databinding.ItemReplayBinding

/** Lists the replays in the library. */
class ReplayAdapter(
    private val onClick: (ReplayStore.Replay) -> Unit,
    private val onLongClick: (ReplayStore.Replay) -> Unit,
) : RecyclerView.Adapter<ReplayAdapter.ViewHolder>() {

    private var items: List<ReplayStore.Replay> = emptyList()

    fun submit(replays: List<ReplayStore.Replay>) {
        items = replays
        notifyDataSetChanged()
    }

    class ViewHolder(val binding: ItemReplayBinding) : RecyclerView.ViewHolder(binding.root)

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val binding = ItemReplayBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return ViewHolder(binding)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        val replay = items[position]
        val context = holder.itemView.context

        holder.binding.name.text = replay.name
        holder.binding.detail.text = context.getString(
            R.string.replay_detail,
            Formatter.formatShortFileSize(context, replay.sizeBytes),
            DateUtils.getRelativeTimeSpanString(replay.modified).toString(),
        )
        holder.itemView.setOnClickListener { onClick(replay) }
        holder.itemView.setOnLongClickListener {
            onLongClick(replay)
            true
        }
    }

    override fun getItemCount(): Int = items.size
}
