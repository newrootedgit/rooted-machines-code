# Git Commands Reference

## Best Practices for Using Git
After making changes in your local repository, follow these best practices to ensure a smooth workflow:
1. **Regular Commits**: Commit your changes frequently with clear and descriptive messages.
```bash
git add <file> or git add .  (. means all)  # Stage your changes
git commit -m "Add feature X to module Y"
git push origin <branch-name>  (your are probably in main branch so git push origin main) # Push your commits to the remote repository
```
2. **Pull Before Push**: Always pull the latest changes from the remote repository before pushing your commits to avoid conflicts **Only necessary if changes have been made to the branch since your last pull**.
```bash
git fetch origin <branch-name>   # Fetch latest changes from remote
git pull origin <branch-name>
```
3. **Branching**: Use branches for new features or bug fixes to keep the main branch stable.
```bash
git checkout -b feature/new-feature
```



## Basic Configuration
```bash
git config --global user.name "Your Name"
git config --global user.email "your.email@example.com"
```

## Getting Started
```bash
git init                          # Initialize a new repository
git clone <url>                   # Clone an existing repository
```

## Making Changes
```bash
git status                        # Check status of working directory
git add <file>                    # Stage changes
git add .                         # Stage all changes
git commit -m "message"           # Commit staged changes
git push origin <branch-name> (Most likely 'main' or 'master')  # Push changes to remote
```

## Branching
```bash
git branch                        # List branches
git branch <branch-name>          # Create a new branch
git checkout <branch-name>        # Switch to a branch
git checkout -b <branch-name>     # Create and switch to a branch
```

## Updating & Pushing
```bash
git pull                          # Fetch and merge remote changes
git push                          # Push commits to remote
git push origin <branch-name>     # Push specific branch
```

## Viewing History
```bash
git log                           # View commit history
git diff                          # Show unstaged changes
git show <commit-hash>            # Show specific commit
```

## Undoing Changes
```bash
git restore <file>                # Discard changes in working directory
git reset HEAD <file>             # Unstage a file
git revert <commit-hash>          # Create a new commit that undoes changes
```
