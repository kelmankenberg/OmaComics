function parse(raw, readFailed) {
    var status = {
        title: "",
        page: 0,
        pageCount: 0,
        progress: 0,
        inProgress: false,
        available: readFailed !== true
    }
    if (!raw)
        return status
    try {
        if (typeof raw !== "string" || raw.length > 8192)
            throw new Error("Invalid status size")
        var value = JSON.parse(raw)
        if (!value || typeof value !== "object" || Array.isArray(value))
            throw new Error("Invalid status object")
        status.title = typeof value.title === "string" ? value.title.slice(0, 256) : ""
        status.page = Number.isInteger(value.page) && value.page >= 0 ? Math.min(value.page, 100000) : 0
        status.pageCount = Number.isInteger(value.pageCount) && value.pageCount >= 0
            ? Math.min(value.pageCount, 100000) : 0
        status.progress = Number.isInteger(value.progress) && value.progress >= 0 && value.progress <= 100
            ? value.progress : 0
        status.inProgress = typeof value.inProgress === "boolean" ? value.inProgress
            : status.title.length > 0 && status.page > 0 && status.pageCount > 0 && status.page < status.pageCount
    } catch (error) {
        status.available = false
    }
    return status
}
